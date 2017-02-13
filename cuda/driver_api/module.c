/*
 * Copyright (C) 2011 Shinpei Kato
 *
 * Systems Research Lab, University of California at Santa Cruz
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "cuda.h"
#include "gdev_api.h"
#include "gdev_cuda.h"

/**
 * Takes a filename fname and loads the corresponding module module into the
 * current context. The CUDA driver API does not attempt to lazily allocate 
 * the resources needed by a module; if the memory for functions and data 
 * (constant and global) needed by the module cannot be allocated, 
 * cuModuleLoad() fails. The file should be a cubin file as output by nvcc 
 * or a PTX file, either as output by nvcc or handwrtten.
 *
 * Parameters:
 * module - Returned module
 * fname - Filename of module to load
 *
 * Returns:
 * CUDA_SUCCESS, CUDA_ERROR_DEINITIALIZED, CUDA_ERROR_NOT_INITIALIZED, 
 * CUDA_ERROR_INVALID_CONTEXT, CUDA_ERROR_INVALID_VALUE, CUDA_ERROR_NOT_FOUND, 
 * CUDA_ERROR_OUT_OF_MEMORY, CUDA_ERROR_FILE_NOT_FOUND 
 */
CUresult cuModuleLoad(CUmodule *module, const char *fname)
{
	CUresult res;
	struct CUmod_st *mod;
	struct CUctx_st *ctx;
	void *bnc_buf;
	Ghandle handle;

	if (!gdev_initialized)
		return CUDA_ERROR_NOT_INITIALIZED;
	if (!module || !fname)
		return CUDA_ERROR_INVALID_VALUE;
	if (!gdev_ctx_current)
		return CUDA_ERROR_INVALID_CONTEXT;

	ctx = gdev_ctx_current;
	handle = ctx->gdev_handle;

	if (!(mod = MALLOC(sizeof(*mod)))) {
		GDEV_PRINT("Failed to allocate memory for module\n");
		res = CUDA_ERROR_OUT_OF_MEMORY;
		goto fail_malloc_mod;
	}

	/* load the cubin image from the given object file. */
	GDEV_PRINT("DEBUG: try to load cubin %s\n", fname);
	if ((res = gdev_cuda_load_cubin(mod, fname)) != CUDA_SUCCESS) {
		GDEV_PRINT("Failed to load cubin\n");
		goto fail_load_cubin;
	}
	GDEV_PRINT("DEBUG: cubin %s loaded\n", fname);

	/* check compatibility of code and device. */
	if ((ctx->cuda_info.chipset & 0xff) != mod->arch) {
		res = CUDA_ERROR_INVALID_SOURCE;
		goto fail_load_cubin;
	}

	/* construct the kernels based on the cubin data. */
	if ((res = gdev_cuda_construct_kernels(mod, &ctx->cuda_info)) 
		!= CUDA_SUCCESS) {
		GDEV_PRINT("Failed to construct kernels\n");
		goto fail_construct_kernels;
	}
	GDEV_PRINT("DEBUG: cuda kernel constructed\n");

	/* allocate (local) static data memory. */
	if (mod->sdata_size > 0) {
		if (!(mod->sdata_addr = gmalloc(handle, mod->sdata_size))) {
			GDEV_PRINT("Failed to allocate device memory for static data\n");
			res = CUDA_ERROR_OUT_OF_MEMORY;
			goto fail_gmalloc_sdata;
		}
	}
	GDEV_PRINT("DEBUG: static data memory allocated\n");

	/* locate the static data information for each kernel. */
	if ((res = gdev_cuda_locate_sdata(mod)) != CUDA_SUCCESS) {
		GDEV_PRINT("Failed to locate static data\n");
		goto fail_locate_sdata;
	}
	GDEV_PRINT("DEBUG: static data memory located\n");

	/* allocate code and constant memory. */
	if (!(mod->code_addr = gmalloc(handle, mod->code_size))) {
		GDEV_PRINT("Failed to allocate device memory for code\n");
		goto fail_gmalloc_code;
	}
	GDEV_PRINT("DEBUG: constant memory allocated\n");


	/* locate the code information for each kernel. */
	if ((res = gdev_cuda_locate_code(mod)) != CUDA_SUCCESS) {
		GDEV_PRINT("Failed to locate code\n");
		goto fail_locate_code;
	}
	GDEV_PRINT("DEBUG: constant memory located\n");


	/* the following malloc() and memcpy() for bounce buffer could be 
	   removed if we use gmalloc_host() here, but they are just an easy 
	   implementation, and don't really affect performance anyway. */
	if (!(bnc_buf = MALLOC(mod->code_size))) {
		GDEV_PRINT("Failed to allocate host memory for code\n");
		res = CUDA_ERROR_OUT_OF_MEMORY;
		goto fail_malloc_code;
	}
	memset(bnc_buf, 0, mod->code_size);
	GDEV_PRINT("DEBUG: host memory allocated\n");

	if ((res = gdev_cuda_memcpy_code(mod, bnc_buf)) 
		!= CUDA_SUCCESS) {
		GDEV_PRINT("Failed to copy code to host\n");
		goto fail_memcpy_code;
	}
	GDEV_PRINT("DEBUG: code copied to host\n");

	/* transfer the code and constant memory onto the device. */
	GDEV_PRINT("DEBUG: transfer code[%u]@%lp to device\n", mod->code_size, mod->code_addr);
	if (gmemcpy_to_device(handle, mod->code_addr, bnc_buf, mod->code_size)) {
		GDEV_PRINT("Failed to copy code to device\n");
		res = CUDA_ERROR_UNKNOWN;
		goto fail_gmemcpy_code;
	}
	GDEV_PRINT("DEBUG: code transfered to device\n");

	/* free the bounce buffer now. */
	FREE(bnc_buf);

	mod->ctx = ctx;
	*module = mod;

	return CUDA_SUCCESS;

fail_gmemcpy_code:
fail_memcpy_code:
	FREE(bnc_buf);
fail_malloc_code:
fail_locate_code:
	gfree(handle, mod->code_addr);
fail_gmalloc_code:
fail_locate_sdata:
	if (mod->sdata_size > 0)
		gfree(handle, mod->sdata_addr);
fail_gmalloc_sdata:
	gdev_cuda_destruct_kernels(mod);
fail_construct_kernels:
	gdev_cuda_unload_cubin(mod);
fail_load_cubin:
	FREE(mod);
fail_malloc_mod:
	*module = NULL;
	return res;
}

CUresult cuModuleLoadFatBinary(CUmodule *module, const void *fatCubin)
{
	GDEV_PRINT("cuModuleLoadFatBinary: Not Implemented Yet\n");
	return CUDA_SUCCESS;
}

/**
 * Unloads a module hmod from the current context.
 *
 * Parameters:
 * hmod - Module to unload
 *
 * Returns:
 * CUDA_SUCCESS, CUDA_ERROR_DEINITIALIZED, CUDA_ERROR_NOT_INITIALIZED, 
 * CUDA_ERROR_INVALID_CONTEXT, CUDA_ERROR_INVALID_VALUE 
 */
CUresult cuModuleUnload(CUmodule hmod)
{
	CUresult res;
	struct CUmod_st *mod = hmod;
	Ghandle handle;

	if (!gdev_initialized)
		return CUDA_ERROR_NOT_INITIALIZED;
	if (!mod)
		return CUDA_ERROR_INVALID_VALUE;
	if (!gdev_ctx_current)
		return CUDA_ERROR_INVALID_CONTEXT;

	handle = gdev_ctx_current->gdev_handle;

	gfree(handle, mod->code_addr);
	if (mod->sdata_size > 0)
		gfree(handle, mod->sdata_addr);

	if ((res = gdev_cuda_destruct_kernels(mod)) != CUDA_SUCCESS)
		return res;

	if ((res = gdev_cuda_unload_cubin(mod)) != CUDA_SUCCESS)
		return res;

	FREE(mod);

	return CUDA_SUCCESS;
}

/**
 * Returns in *hfunc the handle of the function of name name located in module
 *  hmod. If no function of that name exists, cuModuleGetFunction() returns 
 * CUDA_ERROR_NOT_FOUND.
 *
 * Parameters:
 * hfunc - Returned function handle
 * hmod	- Module to retrieve function from
 * name - Name of function to retrieve
 *
 * Returns:
 * CUDA_SUCCESS, CUDA_ERROR_DEINITIALIZED, CUDA_ERROR_NOT_INITIALIZED, 
 * CUDA_ERROR_INVALID_CONTEXT, CUDA_ERROR_INVALID_VALUE, CUDA_ERROR_NOT_FOUND 
 */
CUresult cuModuleGetFunction(CUfunction *hfunc, CUmodule hmod, const char *name)
{
	CUresult res;
	struct CUfunc_st *func;
	struct CUmod_st *mod = hmod;

	if (!gdev_initialized)
		return CUDA_ERROR_NOT_INITIALIZED;
	if (!gdev_ctx_current)
		return CUDA_ERROR_INVALID_CONTEXT;
	if (!hfunc || !mod || !name)
		return CUDA_ERROR_INVALID_VALUE;

	if ((res = gdev_cuda_search_function(&func, mod, name)) != CUDA_SUCCESS)
		return res;

	*hfunc = func;

	return CUDA_SUCCESS;
}

CUresult cuModuleLoadData(CUmodule *module, const void *image)
{
	GDEV_PRINT("cuModuleLoadData: Not Implemented Yet\n");
	return CUDA_SUCCESS;
}

CUresult cuModuleLoadDataEx(CUmodule *module, const void *image, unsigned int numOptions, CUjit_option *options, void **optionValues)
{
	GDEV_PRINT("cuModuleLoadDataEx: Not Implemented Yet\n");
	return CUDA_SUCCESS;
}

/**
 * Returns in *dptr and *bytes the base pointer and size of the global of name 
 * name located in module hmod. If no variable of that name exists, 
 * cuModuleGetGlobal() returns CUDA_ERROR_NOT_FOUND. Both parameters dptr and
 * bytes are optional. If one of them is NULL, it is ignored.
 *
 * Parameters:
 * dptr 	- Returned global device pointer
 * bytes 	- Returned global size in bytes
 * hmod 	- Module to retrieve global from
 * name 	- Name of global to retrieve
 *
 * Returns:
 * CUDA_SUCCESS, CUDA_ERROR_DEINITIALIZED, CUDA_ERROR_NOT_INITIALIZED, 
 * CUDA_ERROR_INVALID_CONTEXT, CUDA_ERROR_INVALID_VALUE, CUDA_ERROR_NOT_FOUND 
 */
CUresult cuModuleGetGlobal
(CUdeviceptr *dptr, unsigned int *bytes, CUmodule hmod, const char *name)
{
	CUresult res;
	uint64_t addr;
	uint32_t size;
	struct CUmod_st *mod = hmod;

	if (!gdev_initialized)
		return CUDA_ERROR_NOT_INITIALIZED;
	if (!gdev_ctx_current)
		return CUDA_ERROR_INVALID_CONTEXT;
	if (!dptr || !bytes || !mod || !name)
		return CUDA_ERROR_INVALID_VALUE;

	if ((res = gdev_cuda_search_symbol(&addr, &size, mod, name)) 
		!= CUDA_SUCCESS)
		return res;

	*dptr = addr;
	*bytes = size;

	return CUDA_SUCCESS;
}

CUresult cuModuleGetTexRef(CUtexref *pTexRef, CUmodule hmod, const char *name)
{
	GDEV_PRINT("cuModuleGetTexRef: Not Implemented Yet\n");
	return CUDA_SUCCESS;
}
