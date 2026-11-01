/* Conformance harness (area hwencoders, spec S-050): hand-written stand-in for ffnvcodec/dynlink_cuda.h.
 * Only the type names plugins/obs-nvenc/cuda-helpers.h uses; no CUDA semantics. Test tooling. */
#pragma once
#include <stddef.h>
#define CUDAAPI
typedef int CUresult;
typedef void *CUcontext;
typedef void *CUarray;
typedef void *CUgraphicsResource;
typedef CUresult CUDAAPI tcuInit(void);
typedef CUresult CUDAAPI tcuDeviceGetCount(void);
typedef CUresult CUDAAPI tcuDeviceGet(void);
typedef CUresult CUDAAPI tcuDeviceGetAttribute(void);
typedef CUresult CUDAAPI tcuCtxCreate_v2(void);
typedef CUresult CUDAAPI tcuCtxDestroy_v2(void);
typedef CUresult CUDAAPI tcuCtxPushCurrent_v2(void);
typedef CUresult CUDAAPI tcuCtxPopCurrent_v2(void);
typedef CUresult CUDAAPI tcuArray3DCreate(void);
typedef CUresult CUDAAPI tcuArrayDestroy(void);
typedef CUresult CUDAAPI tcuMemcpy2D_v2(void);
typedef CUresult CUDAAPI tcuGetErrorName(void);
typedef CUresult CUDAAPI tcuGetErrorString(void);
typedef CUresult CUDAAPI tcuGLGetDevices_v2(void);
typedef CUresult CUDAAPI tcuGraphicsGLRegisterImage(void);
typedef CUresult CUDAAPI tcuGraphicsUnregisterResource(void);
typedef CUresult CUDAAPI tcuGraphicsMapResources(void);
typedef CUresult CUDAAPI tcuGraphicsUnmapResources(void);
typedef CUresult CUDAAPI tcuGraphicsSubResourceGetMappedArray(void);
