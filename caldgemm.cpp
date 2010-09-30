/* ============================================================

The source code is property of the Frankfurt Institute for Advanced Studies (FIAS).
None of the material may be copied, reproduced, distributed, republished, downloaded,
displayed, posted or transmitted in any form or by any means, including, but not
limited to, electronic, mechanical, photocopying, recording, or otherwise,
without the prior written permission of FIAS.

Authors:
David Rohr (drohr@jwdt.org)
Matthias Bach (bach@compeng.uni-frankfurt.de)
Matthias Kretz (kretz@compeng.uni-frankfurt.de)

============================================================ */

#include "caldgemm.h"

#define ILKernelName ILKernel
#include "caldgemm.il"
#undef ILKernelName
#define ILKernelName ILKernelALPHA1
#define CALDGEMM_ALPHA1
#include "caldgemm.il"
#undef CALDGEMM_ALPHA1
#undef ILKernelName

#include <syscall.h>
#include <errno.h>
extern "C" {
#include <common.h>
}
#include <math.h>
#include <unistd.h>

#define MPOL_DEFAULT 0
#define MPOL_PREFERRED 1
#define MPOL_BIND 2
#define MPOL_INTERLEAVE 3

template <class T> T mymin(const T a, const T b) {return(a < b ? a : b);}
template <class T> T mymax(const T a, const T b) {return(a > b ? a : b);}

#define CHKERR(cmd, text) if (cmd != CAL_RESULT_OK) {printf("Error '%s' while " text "\n", calGetErrorString());return(1);}
#define WAITFOREVENT(ctx, eventnr) { CALresult r; if (Info->Debug) {printf("\tWaiting for event from context %d...", eventnr); fflush(stdout);} do { r = calCtxIsEventDone(ctx, events[eventnr]); if (r == CAL_RESULT_ERROR) { printf("Error while waiting for event\nError String: %s\n", calGetErrorString()); return(1);} } while (r == CAL_RESULT_PENDING); if (Info->Debug) printf("Done\n");}

calutil::SampleInfo::SampleInfo()
{
    Pin = -4;
    Verify = CAL_FALSE;
    Disassemble = CAL_FALSE;
    PrintILKernel = CAL_FALSE;
    Quiet = CAL_TRUE;
    DeviceNum = 0;
    Width = 1024;
    Height = 4096;
    AutoHeight = CAL_TRUE;
    Iterations = 1;
    DstMemory = 'c';
    VerboseTiming = CAL_FALSE;
    AsyncTiming = CAL_FALSE;
    TabularTiming = CAL_FALSE;
    Debug = CAL_FALSE;
    MultiThread = CAL_TRUE;
    UseGPU = CAL_TRUE;
    UseCPU = CAL_TRUE;
    GPURatio = -1.0;
    DynamicSched = CAL_TRUE;
    MemPolicy = CAL_TRUE;
    DumpMatrix = CAL_FALSE;
    DivideToGPU = CAL_FALSE;
    AsyncDMA = CAL_TRUE;
    m = 0;
    n = 0;
}

void calutil::print_submatrices(double* M, size_t width, size_t height, size_t pitch, size_t subx, size_t suby, size_t stridex, size_t stridey)
{
    printf("Matrix %lld x %lld, Subblocks %lld x %lld, Strides: %lld / %lld\n", width, height, subx, suby, stridex, stridey);
    for (int j = 0;j < height;j += stridey)
    {
	for (int jj = j;jj < j + suby && jj < height;jj++)
	{
	    for (int i = 0;i < width;i += stridex)
	    {
		for (int ii = i;ii < i + subx && ii < width;ii++)
		{
		    printf("%+ 10.3lf\t", M[jj * pitch + ii]);
		}
	    }
	    printf("\n");
	}
    }
    printf("Done\n");
}

#ifdef UNALIGNED_ADDRESSES
#define _mm_load_pd_use _mm_loadu_pd
#else
#define _mm_load_pd_use _mm_load_pd
#endif

#define _mm_store_pd_use _mm_stream_pd
#define CALDGEMM_USE_VEC_MEMCPY_PREFETCH

int caldgemm::divideBuffer(Data* dst, CALdouble* src, CALint width, CALint height, CALint gpu_width, CALint gpu_height, CALint pitch, CALint numBuffers, bool transpose)
{
    if (Info->Debug) printf("\t\tSRC=0x%llx, w: %d, h: %d, pitch: %d (gpuw: %d, gpuh: %d, transpose: %d)\n", src, width, height, pitch, gpu_width, gpu_height, (int) transpose);

    if (Info->DivideToGPU)
    for (CALuint i = 0;i < numBuffers;i++)
    {
        CHKERR(calResMap(&dst[i].v_data, &dst[i].pitch, dst[i].res, 0), "mapping input buffer for buffer division");
	if (((size_t) dst[i].v_data) & (vcpysize - 1))
	{
	    printf("Invalid alignment\n");
	    return(1);
	}
    }
    
    if (transpose)
    {
#if !defined(CALDGEMM_44)
	if (numBuffers <= 4)
	{
	    for (CALint y = 0;y < width;y += 4)
	    {
    		double* saddr = src + (y * pitch);
    		double* saddr2 = src + ((y + 1) * pitch);
    		double* saddr3 = src + ((y + 2) * pitch);
    		double* saddr4 = src + ((y + 3) * pitch);

		double* daddr = dst[0].d_data + y;
		double* daddr2 = dst[1 % numBuffers].d_data + (1 / numBuffers) * gpu_width + y;
		double* daddr3 = dst[2 % numBuffers].d_data + (2 / numBuffers) * gpu_width + y;
		double* daddr4 = dst[3 % numBuffers].d_data + (3 / numBuffers) * gpu_width + y;
		
		const int dpitch = 4 / numBuffers * gpu_width;
		
		for (int i = 0;i < height;i += 4)
		{
#ifdef CALDGEMM_USE_VEC_MEMCPY_PREFETCH
		    //Prefetching disabled as it currently has a negative performance impact
    		    /*_mm_prefetch(saddr + 100, _MM_HINT_NTA);
    		    _mm_prefetch(saddr2 + 100, _MM_HINT_NTA);
    		    _mm_prefetch(saddr3 + 100, _MM_HINT_NTA);
    		    _mm_prefetch(saddr4 + 100, _MM_HINT_NTA);*/
#endif
		    __m128d x1, x2, x3, x4, x5, x6, x7, x8, x9, x10;
		    x1 = _mm_load_pd_use(saddr);
		    x3 = _mm_load_pd_use(saddr + 2);
		    x2 = _mm_load_pd_use(saddr2);
		    x4 = _mm_load_pd_use(saddr2 + 2);
		    x5 = _mm_load_pd_use(saddr3);
		    x7 = _mm_load_pd_use(saddr3 + 2);
		    x6 = _mm_load_pd_use(saddr4);
		    x8 = _mm_load_pd_use(saddr4 + 2);
		    
		    x9 = _mm_unpacklo_pd(x1, x2);
		    x10 = _mm_unpackhi_pd(x1, x2);
		    x1 = _mm_unpacklo_pd(x3, x4);
		    x2 = _mm_unpackhi_pd(x3, x4);
		    x3 = _mm_unpacklo_pd(x5, x6);
		    x4 = _mm_unpackhi_pd(x5, x6);
		    x5 = _mm_unpacklo_pd(x7, x8);
		    x6 = _mm_unpackhi_pd(x7, x8);
		    
		    _mm_store_pd_use(daddr, x9);
		    _mm_store_pd_use(daddr2, x10);
		    _mm_store_pd_use(daddr + 2, x3);
		    _mm_store_pd_use(daddr2 + 2, x4);
		    _mm_store_pd_use(daddr3, x1);
		    _mm_store_pd_use(daddr4, x2);
		    _mm_store_pd_use(daddr3 + 2, x5);
		    _mm_store_pd_use(daddr4 + 2, x6);
		    
		    saddr += 4;
		    saddr2 += 4;
		    saddr3 += 4;
		    saddr4 += 4;
		    
		    daddr += dpitch;
		    daddr2 += dpitch;
		    daddr3 += dpitch;
		    daddr4 += dpitch;
		}
	    }
	}
	else
#endif
	for (CALint y=0; y < width; y += 2)
	{
    	    double* saddr = src + (y * pitch);
    	    double* saddr2 = src + ((y + 1) * pitch);
        
    	    for (int i = 0;i < height;i += 2)
    	    {
#if defined(CALDGEMM_44) & defined(CALDGEMM_TRANSPOSED_B)
		CALint bank = (i / 2) % 2;
		double* daddr = dst[bank].d_data + (i / 4) * gpu_width * 2 + y * 2;
		double* daddr2 = dst[bank].d_data + (i / 4) * gpu_width * 2 + y * 2 + 2;
#elif defined(CALDGEMM_44) & defined(CALDGEMM_TRANSPOSED_A)
		//Col Interleaved Storage, Numbuffers is either 2 or 4, might be optimized in 2 branches
    		CALint bank = (y / 2) % numBuffers;
#ifdef CALDGEMM_DIAGONAL_TEXTURE
    		double* daddr = dst[bank].d_data + i * gpu_width / 2 + (((y / 2) & 0xFFFFFFFE) + 2 * i) % (gpu_width / 2);
    		double* daddr2 = dst[bank].d_data + (i + 1) * gpu_width / 2 + (((y / 2) & 0xFFFFFFFE) + 2 * i + 2) % (gpu_width / 2);
#else
    		double* daddr = dst[bank].d_data + (i * gpu_width / numBuffers + ((y / numBuffers) & 0xFFFFFFFE));
    		double* daddr2 = dst[bank].d_data + ((i + 1) * gpu_width / numBuffers + ((y / numBuffers) & 0xFFFFFFFE));
#endif
#else
		//Standard Storage
    		CALint bank = (i) % numBuffers;
    		CALint bank2 = (i + 1) % numBuffers;
    		double* daddr = dst[bank].d_data + (i / numBuffers) * gpu_width + y;
    		double* daddr2 = dst[bank2].d_data + (i / numBuffers) * gpu_width + y;
#endif

#ifdef CALDGEMM_USE_VEC_MEMCPY_PREFETCH
    		_mm_prefetch(saddr + 100, _MM_HINT_NTA);
    		_mm_prefetch(saddr2 + 100, _MM_HINT_NTA);
#endif
		__m128d x1, x2, x3, x4;
		x1 = _mm_load_pd_use(saddr);
		x2 = _mm_load_pd_use(saddr2);
		x3 = _mm_unpacklo_pd(x1, x2);
		x4 = _mm_unpackhi_pd(x1, x2);
    		_mm_store_pd_use(daddr, x3);
    		_mm_store_pd_use(daddr2, x4);
    		saddr += 2;
    		saddr2 += 2;
    	    }
    	}
    }
    else
    {
#if defined(CALDGEMM_44) & defined(CALDGEMM_TRANSPOSED_B)
	//Row / Col Interleaved Storage with 2 rows stored in one col
	for (CALint y = 0;y < height / 2;y++)
	{
	    double* daddr = dst[y % 2].d_data + y / 2 * gpu_width * 2;
	    double* saddr = src + 2 * y * pitch;
	    double* saddr2 = src + (2 * y + 1) * pitch;
	    for (int i = 0;i < width;i += 4)
	    {
#ifdef CALDGEMM_USE_VEC_MEMCPY_PREFETCH
    		_mm_prefetch(saddr + 60, _MM_HINT_NTA);
    		_mm_prefetch(saddr2 + 60, _MM_HINT_NTA);
#endif
    		_mm_store_pd_use(daddr + 0, _mm_load_pd_use(saddr));
    		_mm_store_pd_use(daddr + 2, _mm_load_pd_use(saddr2));
    		_mm_store_pd_use(daddr + 4, _mm_load_pd_use(saddr + 2));
    		_mm_store_pd_use(daddr + 6, _mm_load_pd_use(saddr2 + 2));
    		daddr += 8;
    		saddr += 4;
    		saddr2 += 4;
	    }
	}

#elif defined(CALDGEMM_44) & defined(CALDGEMM_TRANSPOSED_A)
	//Col Interleaved Storage for transposed A with 4x4, 8x4 and 8x8 tiling
	if (numBuffers == 4)
	{
	    double* daddr = dst[0].d_data;
	    double* daddr2 = dst[1].d_data;
	    double* daddr3 = dst[2].d_data;
	    double* daddr4 = dst[3].d_data;
	    for (CALint y=0; y < height; y++)
	    {
		int count = dst[0].DataSize * width;
	        double* saddr = src + (y * pitch);
        
	        for (int i = 0;i < count;i += 256)
		{
#ifdef CALDGEMM_USE_VEC_MEMCPY_PREFETCH
    		    _mm_prefetch(saddr + 30, _MM_HINT_NTA);
#endif
    		    _mm_store_pd_use(daddr, _mm_load_pd_use(saddr));
    		    _mm_store_pd_use(daddr2, _mm_load_pd_use(saddr + 2));
    		    _mm_store_pd_use(daddr3, _mm_load_pd_use(saddr + 4));
    		    _mm_store_pd_use(daddr4, _mm_load_pd_use(saddr + 6));
    		    _mm_store_pd_use(daddr + 2, _mm_load_pd_use(saddr + 8));
    		    _mm_store_pd_use(daddr2 + 2, _mm_load_pd_use(saddr + 10));
    		    _mm_store_pd_use(daddr3 + 2, _mm_load_pd_use(saddr + 12));
    		    _mm_store_pd_use(daddr4 + 2, _mm_load_pd_use(saddr + 14));
    		    _mm_store_pd_use(daddr + 4, _mm_load_pd_use(saddr + 16));
    		    _mm_store_pd_use(daddr2 + 4, _mm_load_pd_use(saddr + 18));
    		    _mm_store_pd_use(daddr3 + 4, _mm_load_pd_use(saddr + 20));
    		    _mm_store_pd_use(daddr4 + 4, _mm_load_pd_use(saddr + 22));
    		    _mm_store_pd_use(daddr + 6, _mm_load_pd_use(saddr + 24));
    		    _mm_store_pd_use(daddr2 + 6, _mm_load_pd_use(saddr + 26));
    		    _mm_store_pd_use(daddr3 + 6, _mm_load_pd_use(saddr + 28));
    		    _mm_store_pd_use(daddr4 + 6, _mm_load_pd_use(saddr + 30));
    		    saddr += 32;
    		    daddr += 8;
    		    daddr2+= 8;
    		    daddr3 += 8;
    		    daddr4 += 8;
    		}
    		daddr += (gpu_width - width) / numBuffers;
    		daddr2 += (gpu_width - width) / numBuffers;
    		daddr3 += (gpu_width - width) / numBuffers;
    		daddr4 += (gpu_width - width) / numBuffers;
    	    }
        }
        else
        {
	    double* daddr = dst[0].d_data;
	    double* daddr2 = dst[1].d_data;
	    for (CALint y=0; y < height; y++)
	    {
		int count = dst[0].DataSize * width;
    	        double* saddr = src + (y * pitch);
        
    		for (int i = 0;i < count;i += 64)
    	        {
#ifdef CALDGEMM_USE_VEC_MEMCPY_PREFETCH
    		    _mm_prefetch(saddr + 76, _MM_HINT_NTA);
#endif
    		    _mm_store_pd_use(daddr, _mm_load_pd_use(saddr));
    		    _mm_store_pd_use(daddr2, _mm_load_pd_use(saddr + 2));
    		    _mm_store_pd_use(daddr + 2, _mm_load_pd_use(saddr + 4));
    		    _mm_store_pd_use(daddr2 + 2, _mm_load_pd_use(saddr + 6));
    		    saddr += 8;
    		    daddr += 4;
    		    daddr2+= 4;
    		}
    		daddr += (gpu_width - width) / numBuffers;
    		daddr2 += (gpu_width - width) / numBuffers;
    	    }
        }
#else
	// Array to store the position from which data will be filled in the various output buffers.
	CALint* position = new CALint[numBuffers];
        memset((CALvoid*) position, 0, numBuffers * sizeof(CALint));
	for (CALint y=0; y < height; y++)
	{
    	    CALint bank = y % numBuffers;
    	    double* daddr = dst[bank].d_data + position[bank];
    	    double* saddr = src + (y * pitch);
    	    int count = dst[bank].DataSize * width;
        
    	    for (int i = 0;i < count;i += 64)
    	    {
#ifdef CALDGEMM_USE_VEC_MEMCPY_PREFETCH
    		_mm_prefetch(saddr + 100, _MM_HINT_NTA);
#endif
    		_mm_store_pd_use(daddr, _mm_load_pd_use(saddr));
    		_mm_store_pd_use(daddr + 2, _mm_load_pd_use(saddr + 2));
    		_mm_store_pd_use(daddr + 4, _mm_load_pd_use(saddr + 4));
    		_mm_store_pd_use(daddr + 6, _mm_load_pd_use(saddr + 6));
    		saddr += 8;
    		daddr += 8;
    	}
        
    	    position[bank] += gpu_width;
	}
	delete[] position;
#endif
    }

    if (Info->DivideToGPU)
    for (CALuint i = 0;i < numBuffers;i++)
    {
        CHKERR(calResUnmap(dst[i].res), "unmapping input buffer for buffer division");
    }
    return(0);
}

int caldgemm::mergeBuffers(CALdouble* dst, Data* src, CALint width, CALint height, CALint gpu_width, CALint gpu_height, CALint pitch, CALint numBuffers)
{
    // Array to store the position from which data will be pulled in from the input buffers
    CALint* position = new CALint[numBuffers];
    memset((CALvoid*) position, 0, numBuffers * sizeof(CALint));
    
    if (Info->DstMemory == 'c')
    for (CALuint i = 0;i < cPartsNum;i++)
    {
        CHKERR(calResMap(&src[i].v_data, &src[i].pitch, src[i].res, 0), "mapping output buffer for merging");
	if (((size_t) src[i].v_data) & (vcpysize - 1))
	{
	    printf("Invalid alignment\n");
	    return(1);
	}
    }

    for (CALint y=0; y < height; y++)
    {
#if defined(CALDGEMM_44) & !defined(CALDGEMM_USE_MEMEXPORT)
	CALint bank = y % 4;
	double* saddr2 = src[bank + 4].d_data + position[bank];
	double* paddr2 = src[(y + 1) % 4 + 4].d_data + position[(y + 1) % 4];
#else
        CALint bank = y % numBuffers;
#endif

        double* daddr = dst + (y * pitch);
        double* saddr = src[bank].d_data + position[bank];
        double* paddr = src[(y + 1) % 4].d_data + position[(y + 1) % 4];
        int count = src[bank].DataSize * width;
        
#if defined(CALDGEMM_44) & !defined(CALDGEMM_USE_MEMEXPORT)

        if (__fpclassify(Beta) == FP_ZERO)
        {
    	    for (int i = 0;i < count;i += 128)
    	    {
#ifdef CALDGEMM_USE_VEC_MEMCPY_PREFETCH
    		_mm_prefetch(saddr + 50, _MM_HINT_NTA);
    		_mm_prefetch(saddr2 + 50, _MM_HINT_NTA);
#endif
    		_mm_store_pd_use(daddr, _mm_load_pd(saddr));
    		_mm_store_pd_use(daddr + 2, _mm_load_pd(saddr2));
    	        _mm_store_pd_use(daddr + 4, _mm_load_pd(saddr + 2));
    	        _mm_store_pd_use(daddr + 6, _mm_load_pd(saddr2 + 2));
    		_mm_store_pd_use(daddr + 8, _mm_load_pd(saddr + 4));
    		_mm_store_pd_use(daddr + 10, _mm_load_pd(saddr2 + 4));
    	        _mm_store_pd_use(daddr + 12, _mm_load_pd(saddr + 6));
    	        _mm_store_pd_use(daddr + 14, _mm_load_pd(saddr2 + 6));
    		saddr += 8;
    		saddr2 += 8;
    	        daddr += 16;
    	    }
        }
        else
        {
#undef _mm_store_pd_use
#define _mm_store_pd_use _mm_store_pd
    	    __m128d beta = _mm_set1_pd(Beta);
    	    for (int i = 0;i < count;i += 128)
    	    {
#ifdef CALDGEMM_USE_VEC_MEMCPY_PREFETCH
//    		_mm_prefetch(saddr + 50, _MM_HINT_NTA);
//    		_mm_prefetch(saddr2 + 50, _MM_HINT_NTA);
    	        _m_prefetchw(daddr + 50);
//    	        _mm_prefetch(daddr + 50, _MM_HINT_NTA);
#endif
    		_mm_store_pd_use(daddr, _mm_add_pd(_mm_load_pd(saddr), _mm_mul_pd(beta, _mm_load_pd(daddr))));
    	        _mm_store_pd_use(daddr + 4, _mm_add_pd(_mm_load_pd(saddr + 2), _mm_mul_pd(beta, _mm_load_pd(daddr + 4))));
    		_mm_store_pd_use(daddr + 8, _mm_add_pd(_mm_load_pd(saddr + 4), _mm_mul_pd(beta, _mm_load_pd(daddr + 8))));
    	        _mm_store_pd_use(daddr + 12, _mm_add_pd(_mm_load_pd(saddr + 6), _mm_mul_pd(beta, _mm_load_pd(daddr + 12))));
    		_mm_store_pd_use(daddr + 2, _mm_add_pd(_mm_load_pd(saddr2), _mm_mul_pd(beta, _mm_load_pd(daddr + 2))));
    	        _mm_store_pd_use(daddr + 6, _mm_add_pd(_mm_load_pd(saddr2 + 2), _mm_mul_pd(beta, _mm_load_pd(daddr + 6))));
    		_mm_store_pd_use(daddr + 10, _mm_add_pd(_mm_load_pd(saddr2 + 4), _mm_mul_pd(beta, _mm_load_pd(daddr + 10))));
    	        _mm_store_pd_use(daddr + 14, _mm_add_pd(_mm_load_pd(saddr2 + 6), _mm_mul_pd(beta, _mm_load_pd(daddr + 14))));
    		saddr += 8;
    		saddr2 += 8;
/*    		paddr += 8;
    		paddr2 += 8;*/
    	        daddr += 16;
    	    }
    	}
    	    
	position[bank] += gpu_width / 2;
#else        
        if (__fpclassify(Beta) == FP_ZERO)
        {
#undef _mm_store_pd_use
#define _mm_store_pd_use _mm_stream_pd
    	    for (int i = 0;i < count;i += 64)
    	    {
#ifdef CALDGEMM_USE_VEC_MEMCPY_PREFETCH
    		_mm_prefetch(saddr + 100, _MM_HINT_NTA);
#endif
    		_mm_store_pd_use(daddr, _mm_load_pd(saddr));
    	        _mm_store_pd_use(daddr + 2, _mm_load_pd(saddr + 2));
    		_mm_store_pd_use(daddr + 4, _mm_load_pd(saddr + 4));
    	        _mm_store_pd_use(daddr + 6, _mm_load_pd(saddr + 6));
    		saddr += 8;
    	        daddr += 8;
    	    }
        }
        else
        {
#undef _mm_store_pd_use
#define _mm_store_pd_use _mm_store_pd
    	    __m128d beta = _mm_set1_pd(Beta);
    	    for (int i = 0;i < count;i += 64)
    	    {
#ifdef CALDGEMM_USE_VEC_MEMCPY_PREFETCH
    		_mm_prefetch(saddr + 100, _MM_HINT_NTA);
    	        _m_prefetchw(daddr + 100);
#endif
    		_mm_store_pd_use(daddr, _mm_add_pd(_mm_load_pd(saddr), _mm_mul_pd(beta, _mm_load_pd(daddr))));
    	        _mm_store_pd_use(daddr + 2, _mm_add_pd(_mm_load_pd(saddr + 2), _mm_mul_pd(beta, _mm_load_pd(daddr + 2))));
    		_mm_store_pd_use(daddr + 4, _mm_add_pd(_mm_load_pd(saddr + 4), _mm_mul_pd(beta, _mm_load_pd(daddr + 4))));
    	        _mm_store_pd_use(daddr + 6, _mm_add_pd(_mm_load_pd(saddr + 6), _mm_mul_pd(beta, _mm_load_pd(daddr + 6))));
    		saddr += 8;
    	        daddr += 8;
    	    }
    	}
    	
        position[bank] += gpu_width;
#endif //CALDGEMM_44
    }

    if (Info->DstMemory == 'c')
    for (CALuint i = 0;i < cPartsNum;i++)
    {
        CHKERR(calResUnmap(src[i].res), "unmapping output buffer for merging");
    }
    delete[] position;
    return(0);
}

int caldgemm::InitCALDGEMM(SampleInfo* pInfo)
{
    Info = pInfo;
    gethostname(hostname, 255);
    sched_getaffinity(0, sizeof(oldcpumask), &oldcpumask);
    
    if (Info->Pin != -100)
    {
        CPU_ZERO(&gpumask);
        if (Info->Pin < 0)
        {
            for (int i = 0;i < -Info->Pin;i++) CPU_SET(i, &gpumask);
        }
        else
        {
            CPU_SET(Info->Pin, &gpumask);
        }
	if (Info->Debug) printf("Setting affinitiy to restrict on CPU %d\n", Info->Pin);
	if (0 != sched_setaffinity(0, sizeof(gpumask), &gpumask))
	{
    	    printf("Error setting CPU affinity\n");
    	    return(1);
    	}
    }

    if(!ValidateCALRuntime())
    {
        fprintf(stdout, "Error. Could not find a compatible CAL runtime.\n");
	return 0;
    }

    if (Info->Width & 0xf)
    {
        fprintf(stderr, "Only width of size 16 are computable.\n");
        Info->Width = (Info->Width + 0xf) & (~0xf);
    }
    if (Info->Height & 0x7)
    {
        fprintf(stderr, "Only heights with multiple of 8 are computable.\n" );
        Info->Height = (Info->Height + 0x7) & (~0x7);
    }
    
    numInputs = aPartsNum + bPartsNum;
    numOutputs = cPartsNum;
    numConstantBuffers = 1;
    device = 0;

    if (Info->Debug) printf("Initializing CAL\n");
    if (!Initialize(&device, &ctx_main, Info->DeviceNum))
    {
        return 1;
    }

    CALdeviceattribs attribs;
    attribs.struct_size = sizeof(CALdeviceattribs);
    if (calDeviceGetAttribs(&attribs, Info->DeviceNum) != CAL_RESULT_OK)
    {
	printf("Error getting device attributes\n");
        return 1;
    }

    Features.DoublePrecision = CAL_TRUE;
    if(QueryDeviceCaps(Info->DeviceNum, &Features) != CAL_TRUE)
    {
	printf("The Device Number is invalid or this device is not capable of running this sample.");
	return 1;
    }

    for (int i = 0;i < max_bbuffers;i++)
    {
	if (i < 1)
	{
	    if (!SetupKernel(ILKernel, &modules[i][0], &ctx_main, (CALboolean) (Info->Disassemble && i == 0)) ||
		!SetupKernel(ILKernelALPHA1, &modules[i][1], &ctx_main, (CALboolean) (Info->Disassemble && i == 0)))
	    {
		return 1;
	    }
	    for (int j = 0;j < kernel_count;j++) progNames[i][j] = new CALname[numInputs + numOutputs + numConstantBuffers];
	}

	datas[i] = new Data[numInputs + numOutputs + numConstantBuffers];
	resourceHandlers[i] = new CALresource[numInputs + numOutputs + numConstantBuffers];
	if (!SetupData(modules[i], resourceHandlers[i], datas[i], &device, &ctx_main, numInputs, numOutputs, numConstantBuffers, progNames[i], i))
	{
	    if (i < ctxcount)
		return 1;
	    else
		break;
	}
	bbuffers = i + 1;
    
	if (i < ctxcount && Info->MultiThread)
	{
	    for (int j = 0;j < 2;j++) pthread_mutex_init(&mParam[i].mergeMutex[j], NULL);
	    mParam[i].cls = this;
	    mParam[i].nContext = i;
	    mParam[i].terminate = CAL_FALSE;
	    pthread_t thr;
	    pthread_create(&thr, NULL, merge_wrapper, &mParam[i]);
	}
    }
    if (Info->Debug) printf("Was able to allocate %d bbuffers\n", bbuffers);
    if (Info->UseCPU)
    {
	cParam.cls = this;
	cParam.terminate = CAL_FALSE;
        for (int j = 0;j < 2;j++) pthread_mutex_init(&cParam.cblasMutex[j], NULL);
        pthread_mutex_lock(&cParam.cblasMutex[0]);
        if (Info->MultiThread)
        {
    	    pthread_t thr;
	    pthread_create(&thr, NULL, cblas_wrapper, &cParam);
	}
    }
    
    if (Info->MemPolicy)
    {
	unsigned long nodemask = 0xffffff;
	syscall(SYS_set_mempolicy, MPOL_INTERLEAVE, &nodemask, sizeof(nodemask) * 8);
    }

    if (Info->Pin != -100) sched_setaffinity(0, sizeof(oldcpumask), &oldcpumask);

    return(0);
}

void* cblas_wrapper(void* arg)
{
    volatile caldgemm::cblasParameters* par = (caldgemm::cblasParameters*) arg;
    volatile caldgemm::SampleInfo* Info = par->cls->Info;
    
    if (Info->Debug) printf("Cblas helper thread started\n");
    
    sched_setaffinity(0, sizeof(par->cls->oldcpumask), &par->cls->oldcpumask);
    
    if (Info->MultiThread) pthread_mutex_lock(&par->cls->cParam.cblasMutex[1]);
    while (pthread_mutex_lock(&par->cls->cParam.cblasMutex[1]) == 0 && par->terminate == CAL_FALSE)
    {
	const CALdouble Alpha = par->cls->Alpha;
	const CALdouble Beta = par->cls->Beta;
	double* const A = par->cls->A;
	double* const B = par->cls->B;
	double* const C = par->cls->C;
	const size_t A_pitch = par->cls->A_pitch;
	const size_t B_pitch = par->cls->B_pitch;
	const size_t C_pitch = par->cls->C_pitch;
	const size_t A_pitch_use = (par->cls->TransposeA == CblasTrans ? 1 : A_pitch);
	const size_t B_pitch_use = (par->cls->TransposeB == CblasTrans ? B_pitch : 1);
	const CBLAS_TRANSPOSE TransposeA = par->cls->TransposeA;
	const CBLAS_TRANSPOSE TransposeB = par->cls->TransposeB;
	if (!Info->Quiet) printf("\t\tSlave thread starting cblas (m: %lld, n: %lld, cblas_size: %lld, dynamic: %lld/%lld)\n", Info->m, Info->n, par->cblas_size, par->dynamic_run, par->dynamic_size);

	par->cls->Timers.CPUTimer.Start();
	int old_goto_threads = get_num_procs();
	if (Info->Pin != -100)
	{
	    goto_set_num_threads(old_goto_threads - (Info->Pin < 0 ? -Info->Pin : 1));
	    caldgemm_goto_reserve_cpus(Info->Pin < 0 ? -Info->Pin : 1);
	}
	
	if (par->dynamic_run)
	{
	    if (par->cls->gpu_m >= par->cls->gpu_n)
	    {
		cblas_dgemm(CblasRowMajor, TransposeA, TransposeB, par->dynamic_run, par->dynamic_size, Info->Width, Alpha, A + (Info->m - par->cblas_size - par->dynamic_run) * A_pitch_use, A_pitch, B + (Info->n - Info->n % Info->Height - par->dynamic_size) * B_pitch_use, B_pitch, Beta, C + (Info->m - par->cblas_size - par->dynamic_run) * C_pitch + Info->n - Info->n % Info->Height - par->dynamic_size, C_pitch);
	    }
	    else
	    {
		cblas_dgemm(CblasRowMajor, TransposeA, TransposeB, par->dynamic_size, par->dynamic_run, Info->Width, Alpha, A + (Info->m - Info->m % Info->Height - par->dynamic_size) * A_pitch_use, A_pitch, B + (Info->n - par->cblas_size - par->dynamic_run) * B_pitch_use, B_pitch, Beta, C + (Info->m - Info->m % Info->Height - par->dynamic_size) * C_pitch + Info->n - par->cblas_size - par->dynamic_run, C_pitch);
	    }
	}
	
	if (Info->m >= Info->n)	//favor splitting m because of consecutive memory
	{
	    if (par->dynamic_run == 0)
	    {
		cblas_dgemm(CblasRowMajor, TransposeA, TransposeB, par->cblas_size, Info->n, Info->Width, Alpha, A + (Info->m - par->cblas_size) * A_pitch_use, A_pitch, B, B_pitch, Beta, C + (Info->m - par->cblas_size) * C_pitch, C_pitch);
	    }
	    
	    if (Info->n % Info->Height && par->borders_done == CAL_FALSE)
		cblas_dgemm(CblasRowMajor, TransposeA, TransposeB, Info->m - par->cblas_size, Info->n % Info->Height, Info->Width, Alpha, A, A_pitch, B + (Info->n - Info->n % Info->Height) * B_pitch_use, B_pitch, Beta, C + Info->n - Info->n % Info->Height, C_pitch);
	}
	else
	{
	    if (par->dynamic_run == 0)
	    {
		cblas_dgemm(CblasRowMajor, TransposeA, TransposeB, Info->m, par->cblas_size, Info->Width, Alpha, A, A_pitch, B + (Info->n - par->cblas_size) * B_pitch_use, B_pitch, Beta, C + Info->n - par->cblas_size, C_pitch);
	    }
	    
	    if (Info->m % Info->Height && par->borders_done == CAL_FALSE)
		cblas_dgemm(CblasRowMajor, TransposeA, TransposeB, Info->m % Info->Height, Info->n - par->cblas_size, Info->Width, Alpha, A + (Info->m - Info->m % Info->Height) * A_pitch_use, A_pitch, B, B_pitch, Beta, C + (Info->m - Info->m % Info->Height) * B_pitch, C_pitch);
	}
	goto_set_num_threads(old_goto_threads);
	caldgemm_goto_reserve_cpus(0);
	par->cls->Timers.CPUTimer.Stop();

        if (Info->Debug) printf("\t\tUnlocking cblasmutex\n");
        pthread_mutex_unlock(&par->cls->cParam.cblasMutex[0]);
        if (!Info->MultiThread) break;
    }
    if (Info->Debug) printf("blas slave terminating\n");
    if (Info->MultiThread)
    {
	pthread_exit(NULL);
    }
    return(NULL);
}

void* merge_wrapper(void* arg)
{
    caldgemm::mergeParameters* par = (caldgemm::mergeParameters*) arg;
    
    if (par->cls->Info->Debug) printf("Merger Thread %d started\n", par->nContext);
    
    if (par->cls->Info->Pin != -100)
    {
	if (-par->cls->Info->Pin == par->cls->ctxcount + 1)
	{
	    cpu_set_t merge_mask;
	    CPU_ZERO(&merge_mask);
	    CPU_SET(par->nContext + 1, &merge_mask);
	    sched_setaffinity(0, sizeof(cpu_set_t), &merge_mask);
	}
	else
	{
	    sched_setaffinity(0, sizeof(cpu_set_t), &par->cls->gpumask);
	}
    }
    
    pthread_mutex_lock(&par->mergeMutex[1]);
    while (pthread_mutex_lock(&par->mergeMutex[1]) == 0 && par->terminate == CAL_FALSE)
    {
	if (par->cls->Info->Debug) printf("\t\tSlave thread starting merge process\n");
        par->cls->mergeBuffers(par->dst, par->src, par->cls->Info->Height, par->cls->Info->Height, par->cls->BufferHeight, par->cls->BufferHeight, par->cls->C_pitch, par->cls->cPartsNum);
        if (par->cls->Info->Debug) printf("\t\tUnlocking mutex %d (Slavethread)\n", par->nContext);
        pthread_mutex_unlock(&par->mergeMutex[0]);
    }
    if (par->cls->Info->Debug) printf("merge slave %d terminating\n", par->nContext);
    pthread_exit(NULL);
    return(NULL);
}

int calutil::DumpMatrix(double* a, double* b, double* c, double alpha, double beta, int tmp_m, int tmp_k, int tmp_n, int Apitch, int Bpitch, int Cpitch, CBLAS_ORDER order, CBLAS_TRANSPOSE TransA, CBLAS_TRANSPOSE TransB)
{
    int i = 0;
    char filename[256];
    FILE* fp = NULL;
    do
    {
	if (fp) fclose(fp);
	sprintf(filename, "dump%d.out", i++);
    } while ((fp = fopen(filename, "r")) != NULL && i < 100);
    if (i == 100)
    {
	if (fp) fclose(fp);
	return(1);
    }
    fp = fopen(filename, "w+b");
    fwrite(&a, sizeof(a), 1, fp);
    fwrite(&b, sizeof(b), 1, fp);
    fwrite(&c, sizeof(c), 1, fp);
    fwrite(&alpha, sizeof(alpha), 1, fp);
    fwrite(&beta, sizeof(beta), 1, fp);
    fwrite(&tmp_m, sizeof(tmp_m), 1, fp);
    fwrite(&tmp_k, sizeof(tmp_k), 1, fp);
    fwrite(&tmp_n, sizeof(tmp_n), 1, fp);
    fwrite(&Apitch, sizeof(Apitch), 1, fp);
    fwrite(&Bpitch, sizeof(Bpitch), 1, fp);
    fwrite(&Cpitch, sizeof(Cpitch), 1, fp);
    for (i = 0;i < tmp_m;i++)
    {
	fwrite(a + i * Apitch, sizeof(double), tmp_k, fp);
    }
    for (i = 0;i < tmp_k;i++)
    {
	fwrite(b + i * Bpitch, sizeof(double), tmp_n, fp);
    }
    fclose(fp);
    return(0);
}

int caldgemm::RunCALDGEMM(double* a, double* b, double* c, double alpha, double beta, size_t tmp_m, size_t tmp_k, size_t tmp_n, size_t Apitch, size_t Bpitch, size_t Cpitch, CBLAS_ORDER order, CBLAS_TRANSPOSE TransA, CBLAS_TRANSPOSE TransB)
{
    if (tmp_m == 0 || tmp_k == 0 || tmp_n == 0) return(0);		//Do Nothing

/*  //Disable output for all but one host in MPI rin
    if (strcmp(hostname, "gpu-dev05") != 0)
    {
	Info->Debug = CAL_FALSE;
	Info->Quiet = CAL_TRUE;
	Info->Verify = CAL_FALSE;
    }*/

    bool forceCPU = false;
    bool forceReinit = false;
    double GPURatio;
    
    A = a;
    B = b;
    C = c;
    Alpha = alpha;
    Beta = beta;
    if (tmp_m != -1) Info->m = tmp_m;
    if (tmp_n != -1) Info->n = tmp_n;
    if (tmp_k != -1) Info->Width = tmp_k;

    A_pitch = Apitch != -1 ? Apitch : Info->Width;
    B_pitch = Bpitch != -1 ? Bpitch : Info->n;
    C_pitch = Cpitch != -1 ? Cpitch : Info->n;
    ResetTimers();

    if (Info->Debug) printf("Starting DGEMM Run m=%lld k=%lld n=%lld Alpha=%lf Beta=%lf LDA=0x%lx LDB=0x%lx LDC=0x%lx At=%d Bt=%d ColMajor=%d (A=0x%llx, B=0x%llx, C=0x%llx)\n", Info->m, Info->Width, Info->n, Alpha, Beta, A_pitch, B_pitch, C_pitch, (int) (TransA == CblasTrans), (int) (TransB == CblasTrans), (int) (order == CblasColMajor), A, B, C);

    //Check for double == 1.0 is unsafe and causes compiler warning
    const unsigned long long int double_one = 0x3FF0000000000000;	//1.0 in double
    const int kernel_num = (reinterpret_cast<long long int &>(Alpha) == double_one);
    if (kernel_num && Info->Debug) printf("Using Kernel for ALPHA = 1\n");

    Timers.System.Start();
    
    if (order == CblasColMajor)
    {
	double* tmpd;
	size_t tmpi;
	CBLAS_TRANSPOSE tmpt;
	tmpd = A; A = B; B = tmpd;
	tmpi = Info->m; Info->m = Info->n; Info->n = tmpi;
	tmpi = A_pitch; A_pitch = B_pitch; B_pitch = tmpi;
	tmpt = TransA;TransA = TransB;TransB = tmpt;
    }
    
    TransposeA = TransA;
    TransposeB = TransB;    
    
    if (Info->Verify)
    {
	D = new CALdouble[(size_t) Info->m * (size_t) C_pitch];
	if (D == NULL)
	{
	    printf("Memory allocation error\n");
	    return(1);
	}
	memcpy(D, C, Info->m * C_pitch * sizeof(CALdouble));
    }

    if (Info->DumpMatrix) DumpMatrix(A, B, C, Alpha, Beta, Info->m, Info->Width, Info->n, A_pitch, B_pitch, C_pitch, CblasRowMajor, TransposeA, TransposeB);

#ifndef TESTMODE    
    //Check if the GPU can/shall process the required dgemm task
    if (Info->Iterations > 1);
    else if (Info->Width % 256 || Info->Width < 256) forceCPU = true;
    else if (Info->m < 512 || Info->n < 512) forceCPU = true;
    else if (__fpclassify(Alpha) == FP_ZERO) forceCPU = true;
    else if (((size_t) A) & (vcpysize - 1) || ((size_t) B) & (vcpysize - 1) || ((size_t) C) & (vcpysize - 1) ||
	A_pitch & (vcpysize / sizeof(CALdouble) - 1) || B_pitch & (vcpysize / sizeof(CALdouble) - 1)|| C_pitch & (vcpysize / sizeof(CALdouble) - 1))
    {
	printf("Input addresses not aligned correctly: A 0x%llX B 0x%llX C 0x%llX Pitch 0x%llX 0x%llX 0x%llX\n", A, B, C, A_pitch, B_pitch, C_pitch);
	forceCPU = true;
    }
#endif

    if (Info->AutoHeight)
    {
	if (Info->m < 1024 || Info->n < 1024)
	{
	    Info->Height = 512;
	}
	else if (Info->m < 2048 || Info->n < 2048 || Info->m * Info->n < 16 * 1024 * 1024)
        {
    	    Info->Height = 1024;
	}
        else if (Info->m < 3072 || Info->n < 3072 || Info->m * Info->n < 120 * 1024 * 1024)
	{
	    Info->Height = 2048;
	}
        else if (Info->m < 4096 || Info->n < 4096 || Info->m * Info->n < 400 * 1024 * 1024)
	{
	    Info->Height = 3072;
	}
	else
	{
	    Info->Height = 4096;
	}
	if (Info->Height != BufferHeight)
	{
	    if (!Info->Quiet) printf("Using reduced Height %lld instead of  %lld\n", Info->Height, BufferHeight);
	}
    }
    
    if (Info->Width > BufferWidth || Info->Height > BufferHeight) forceReinit = true;

    if (Info->UseGPU == CAL_FALSE || Info->m < Info->Height || Info->n < Info->Height || (forceReinit && (long long int) Info->m * (long long int) Info->n * (long long int) Info->Width < (long long int) 24 * 1024 * 1024 * 1024) || (Info->Width < 1024 && Info->Height < 1024)) forceCPU = true;
    
/*  //Run on CPU on all but one node in MPIRUN
    if (strcmp(hostname, "gpu-dev05") != 0)
    {
	printf("Hostname not 5 but %s\n", hostname);
	forceCPU = true;
    }*/

    if (forceCPU)
    {
	if (Info->Debug) printf("Running CPU only DGEMM\n");
	Timers.CPUTimer.Start();
	cblas_dgemm(CblasRowMajor, TransposeA, TransposeB, Info->m, Info->n, Info->Width, Alpha, A, A_pitch, B, B_pitch, Beta, C, C_pitch);
	Timers.CPUTimer.Stop();
	CPUOnlyRun = true;
	goto RunCALDGEMM_end;
    }
    CPUOnlyRun = false;
    
    if (Info->Pin != -100)
    {
	if (-Info->Pin == ctxcount + 1)
	{
	    cpu_set_t divide_mask;
	    CPU_ZERO(&divide_mask);
	    CPU_SET(0, &divide_mask);
	    sched_setaffinity(0, sizeof(cpu_set_t), &divide_mask);
	}
	else
	{
	    sched_setaffinity(0, sizeof(cpu_set_t), &gpumask);
	}
    }
    
    if (forceReinit)
    {
	if (Info->Debug) printf("Reinit for biffer width / height\n");
	for (int i = 0;i < bbuffers;i++)
	{
    	    CleanupData(&ctx_main, resourceHandlers[i], datas[i], numInputs + numOutputs + numConstantBuffers, i);
	    SetupData(modules[i], resourceHandlers[i], datas[i], &device, &ctx_main, numInputs, numOutputs, numConstantBuffers, progNames[i], i);
	}
    }

    if (Info->Debug) printf("Initiliazing GPU Constant Buffers...");
    for (int i = 0;i < 1;i++)
    {
	if (Info->Debug) printf("%d", i);
	datas[i][aPartsNum + bPartsNum].d_data[3] = alpha;
	if (CopyDataToGPU(&ctx_main, resourceHandlers[i] + numInputs, datas[i] + numInputs, numConstantBuffers, CAL_TRUE, &events[i])) return(1);
    }
    if (Info->Debug) printf("   Done\n");
    
    if (Info->GPURatio < 0)
    {
	//Optimal ratio found using combined runs
	     if ((long long int) Info->m * (long long int) Info->n > (long long int) 5000000000) GPURatio = 0.76;
	else if ((long long int) Info->m * (long long int) Info->n > (long long int) 4500000000) GPURatio = 0.74;
	else if ((long long int) Info->m * (long long int) Info->n > (long long int) 3500000000) GPURatio = 0.73;
	else if ((long long int) Info->m * (long long int) Info->n > (long long int) 2500000000) GPURatio = 0.72;
	else if ((long long int) Info->m * (long long int) Info->n > (long long int) 200000000) GPURatio = 0.71;
	else if ((long long int) Info->m * (long long int) Info->n > (long long int) 13000000) GPURatio = 0.70;
	else if ((long long int) Info->m * (long long int) Info->n > (long long int) 8000000) GPURatio = 0.69;
	else if ((long long int) Info->m * (long long int) Info->n > (long long int) 6000000) GPURatio = 0.65;
	else if ((long long int) Info->m * (long long int) Info->n > (long long int) 3000000) GPURatio = 0.60;
	else GPURatio = 0.50;
	GPURatio *= (double) Info->Width / (double) 1024;
	if (Info->Height < 1024) GPURatio *= (double) Info->Height / (double) 1024 * (double) Info->Height / (double) 1024;
	if (Info->Debug) printf("GPURatio automatically set to %1.2lf\n", GPURatio);
    }
    else
    {
	GPURatio = Info->GPURatio;
    }
    
    cParam.dynamic_run = 0;
    cParam.borders_done = CAL_FALSE;
    if (Info->UseCPU == CAL_TRUE && Info->UseGPU == CAL_TRUE)
    {
	if (Info->m >= Info->n)
	{
	    const size_t virtualm = Info->m + (Info->n % Info->Height) * Info->m / Info->n;
	    gpu_m = GPURatio * (float) virtualm + (Info->Height - 1);
	    if (gpu_m > Info->m) gpu_m = Info->m;
	    gpu_m -= gpu_m % Info->Height;
	    cParam.cblas_size = Info->m - gpu_m;
	    gpu_n = Info->n;
	    gpu_n -= gpu_n % Info->Height;
	    if (Info->Debug) printf("Splitting: GPU: %lld x %lld, CPU: %lld x %lld\n", gpu_m, gpu_n, Info->m - gpu_m, gpu_n);
	}
        else
        {
    	    const size_t virtualn = Info->n + (Info->m % Info->Height) * Info->n / Info->m;
	    gpu_n = GPURatio * (float) virtualn + (Info->Height - 1);
	    if (gpu_n > Info->n) gpu_n = Info->n;
	    gpu_n -= gpu_n % Info->Height;
	    cParam.cblas_size = Info->n - gpu_n;
	    gpu_m = Info->m;
	    gpu_m -= gpu_m % Info->Height;
	    if (Info->Debug) printf("Splitting: GPU: %lld x %lld, CPU: %lld x %lld\n", gpu_m, gpu_n, Info->m, Info->n - gpu_n);
	}
	if (cParam.cblas_size == 0 && Info->DynamicSched == CAL_TRUE)
	{
	    cParam.dynamic_run = Info->Height;
	    cParam.dynamic_size = mymin((int) (Info->m >= Info->n ? Info->m : Info->n), (int) ((1.0f - GPURatio) * (float) Info->m * Info->n / Info->Height));
	    cParam.dynamic_size -= cParam.dynamic_size % Info->Height;
	    if (!Info->Quiet) printf("Scheduling initial dynamic run over %lldx%lld blocks\n", cParam.dynamic_run, cParam.dynamic_size);
	}
    }
    else
    {
	if (Info->n % Info->Height || Info->m % Info->Height)
	{
	    printf("Invalid matrix size for GPU only\n");
	    return(1);
	}
	gpu_n = Info->n;
	gpu_m = Info->m;
    }
    
    if (Info->UseCPU)
    {
	pthread_mutex_unlock(&cParam.cblasMutex[1]);
	if (!Info->MultiThread)
	{
	    cblas_wrapper((void*) &cParam);
	}
    }

    Timers.GPUTimer.Start();

    for (CALuint i = 0; i < Info->Iterations; ++i)
    {
	int oldj;
	int j = 0;
	
	const size_t mb = gpu_m / Info->Height;
	const size_t nb = gpu_n / Info->Height;
	size_t blockm, blockn, lastm, lastn;
	size_t nBlocks = mb * nb;
	
	if (!Info->Quiet && (buffersSwitchable ? mymin(nb, mb) : nb) > bbuffers) printf("Insufficient buffers for Input Matrices, retransfer required\n");
	
	if (gpu_n && gpu_m)
	for (size_t k = 0;k <= nBlocks;k++)
	{
	    size_t newblockm, newblockn;
	    if (k < nBlocks)
	    {
		DGEMM_getblocks(k, newblockm, newblockn);
		if (cParam.dynamic_run)
		{
		    if (newblockm * Info->Height >= gpu_m - cParam.dynamic_run && newblockn * Info->Height >= gpu_n - cParam.dynamic_size)
		    {
			if (Info->Debug) printf("GPU skipping k = %lld\n", k);
			continue;
		    }
		}
	    }
	
	    lastm = blockm;
	    lastn = blockn;
	    if (k < nBlocks)
	    {
		blockn = newblockn;
		blockm = newblockm;
		if (Info->Debug) printf("Iteration k = %lld, m = %lld, n = %lld (Context %d)\n", k, blockm, blockn, j);
		
		if (k <= 1 || ctxcount == 1 || Info->AsyncDMA == CAL_FALSE) DGEMM_prepare(k, j);
	        if (ctxcount > 1 && k >= 1 && Info->AsyncDMA)
	        {
		    size_t nextk = k + 1;
		    size_t nextblockm, nextblockn;
		    DGEMM_getblocks(nextk, nextblockm, nextblockn);
		    if (cParam.dynamic_run)
			while (nextk < nBlocks && nextblockm * Info->Height >= gpu_m - cParam.dynamic_run && nextblockn * Info->Height >= gpu_n - cParam.dynamic_size) nextk++;
		    if (nextk < nBlocks) DGEMM_prepare(nextk, (j + 1) % ctxcount);
		}

	        if (Info->MultiThread)
	        {
		    if (Info->Debug) printf("\tLocking mutex %d\n", j);
		    pthread_mutex_lock(&mParam[j].mergeMutex[0]);
		}
		WAITFOREVENT(ctx_main, j);
	        if (Info->Debug) printf("\tExecuting MM kernel\n");
	        if (gpu_m < gpu_n && buffersSwitchable && bbuffers >= mb)
	        {
		    for (int l = 0;l < aPartsNum;l++) CHKERR(calCtxSetMem(ctx_main, progNames[0][kernel_num][l], datas[blockm][aPartsNum + l].dstMem), "setting kernel memory A");
		    for (int l = 0;l < bPartsNum;l++) CHKERR(calCtxSetMem(ctx_main, progNames[0][kernel_num][aPartsNum + l], datas[blockn % 2][l].dstMem), "setting kernel memory B");
	        }
	        else
	        {
		    for (int l = 0;l < aPartsNum;l++) CHKERR(calCtxSetMem(ctx_main, progNames[0][kernel_num][l], datas[blockm % 2][l].dstMem), "setting kernel memory A");
		    for (int l = aPartsNum;l < aPartsNum + bPartsNum;l++) CHKERR(calCtxSetMem(ctx_main, progNames[0][kernel_num][l], datas[nb > bbuffers ? (blockn % 2) : blockn][l].dstMem), "setting kernel memory B");
		}
	        for (int l = 0;l < cPartsNum;l++) CHKERR(calCtxSetMem(ctx_main, progNames[0][kernel_num][numInputs + numConstantBuffers + l], datas[j][numInputs + numConstantBuffers + l].dstMem), "setting kernel output memroy");
		if (!RunProgram(&ctx_main, &modules[0][kernel_num], Info->Height / TILING_X, Info->Height / TILING_Y, &events[j])) {printf("Error running program\n"); return 1;}
		calCtxFlush(ctx_main);
		
		if (Info->UseCPU && Info->MultiThread && Info->DynamicSched && cParam.dynamic_run == 0 && k >= 0.75f * GPURatio * nBlocks)
		{
		    if (pthread_mutex_trylock(&cParam.cblasMutex[0]) == 0)
		    {
			cParam.dynamic_size = ((1.0f - GPURatio) * (float) (nBlocks - k - 1) + 0.5) * Info->Height;
			if (cParam.dynamic_size > (nBlocks - k - 1) * Info->Height) cParam.dynamic_size = (nBlocks - k - 1) * Info->Height;
			if (cParam.dynamic_size > Info->Height)
			{
    			    cParam.dynamic_run = 1 + cParam.dynamic_size / mymin(gpu_m, gpu_n);
    			    cParam.dynamic_size /= cParam.dynamic_run;
    			    cParam.dynamic_size -= cParam.dynamic_size % Info->Height;
    			    cParam.dynamic_run *= Info->Height;
    			    cParam.borders_done = CAL_TRUE;
    			    if (!Info->Quiet) printf("Scheduling Additional CPU DGEMM Run over %lld blockrows, %lld blocks\n", cParam.dynamic_run / Info->Height, cParam.dynamic_size / Info->Height);
    			    pthread_mutex_unlock(&cParam.cblasMutex[1]);
    			}
    			else
    			{
    			    pthread_mutex_unlock(&cParam.cblasMutex[0]);
    			}
    		    }
    		}
    	    }
    	    if (ctxcount == 1)
    	    {
    		oldj = j;
    		lastm = blockm;
    		lastn = blockn;
    	    }
    	    if ((ctxcount > 1) ? (k > 0) : (k < nBlocks))
    	    {
    		WAITFOREVENT(ctx_main, oldj);
    		if (Info->DstMemory == 'g')
    		{
    	    	    if (Info->VerboseTiming) Timers.CounterCopyFrom.Start();
    	    	    if (Info->Debug == CAL_TRUE) printf("Fething part of C from GPU\n");
    		    if (CopyDataFromGPU(&ctx_main, resourceHandlers[oldj] + numInputs + numConstantBuffers, datas[oldj] + numInputs + numConstantBuffers, numOutputs, &events[oldj])) {printf("Error copying from GPU\n"); return(1);}
    	    	    if (Info->VerboseTiming) Timers.CounterCopyFrom.Stop();
    		    WAITFOREVENT(ctx_main, oldj);
    	    	}
    		if (Info->VerboseTiming) Timers.CounterMerge.Start();
    		if (Info->Debug) printf("\tMerging buffer\n");

		if (k == nBlocks || Info->MultiThread == CAL_FALSE)
		{
	    	    if (mergeBuffers(C + lastn * Info->Height + lastm * C_pitch * Info->Height, datas[oldj] + numInputs + numConstantBuffers, Info->Height, Info->Height, BufferHeight, BufferHeight, C_pitch, cPartsNum)) {printf("Error merging\n"); return(1);}
	    	    if (Info->MultiThread)
	    	    {
	    		pthread_mutex_unlock(&mParam[oldj].mergeMutex[0]);
	    		for (int l = 1;l < ctxcount;l++)
	    		{
	    		    pthread_mutex_lock(&mParam[(oldj + l) % ctxcount].mergeMutex[0]);
	    		    pthread_mutex_unlock(&mParam[(oldj + l) % ctxcount].mergeMutex[0]);
	    		}
	    	    }
	    	}
	    	else
	    	{
		    mParam[oldj].dst = C + (lastn * Info->Height + lastm * C_pitch * Info->Height);
		    mParam[oldj].src = datas[oldj] + numInputs + numConstantBuffers;
		    pthread_mutex_unlock(&mParam[oldj].mergeMutex[1]);
		}

	        if (Info->VerboseTiming) Timers.CounterMerge.Stop();
	    }
	    oldj = j;
    	    j = (j + 1) % ctxcount;
	}
    }
    Timers.GPUTimer.Stop();
    
    if (Info->Pin != -100) sched_setaffinity(0, sizeof(oldcpumask), &oldcpumask);

    if (Info->UseCPU)
    {
	CPerfCounter cpuwait;
	if (!Info->Quiet)
	{
	    cpuwait.Reset();
	    cpuwait.Start();
	}
	pthread_mutex_lock(&cParam.cblasMutex[0]);
	if (!Info->Quiet)
	{
	    cpuwait.Stop();
	    printf("CPU synchronisation took %2.4lf sec\n", cpuwait.GetElapsedTime());
	}
    }

RunCALDGEMM_end:
    Timers.System.Stop();

    if (Info->Debug) printf("DGEMM Run Complete\n");
    
#ifdef TESTMODE
    print_submatrices(C, 12, 24, Info->n, 1, 1, 1, 1);
#endif
    
    if( Info->Quiet == CAL_FALSE && !AnalyzeResults(datas[0]) )
    {
        return 1;
    }
    if (Info->Verify) delete[] D;
    return(0);
}

inline void caldgemm::DGEMM_getblocks(size_t k, size_t &blockm, size_t &blockn)
{
    if (gpu_m >= gpu_n)
    {
        const int nb = gpu_n / Info->Height;
        blockn = k % nb;
        blockm = k / nb;
    }
    else
    {
        const int mb = gpu_m / Info->Height;
        blockm = k % mb;
        blockn = k / mb;
    }
}

int caldgemm::DGEMM_prepare(size_t k, int j)
{
    const size_t nb = gpu_n / Info->Height;
    const size_t mb = gpu_m / Info->Height;
    size_t blockm, blockn;
    DGEMM_getblocks(k, blockm, blockn);
    
    bool buffersSufficiant;
    if (gpu_m >= gpu_n)
    {
	buffersSufficiant = (bbuffers >= nb);
    }
    else
    {
	buffersSufficiant = (bbuffers >= mb && buffersSwitchable);
    }

    if (Info->VerboseTiming) Timers.CounterDivide.Start();
    if (blockn == 0 || (gpu_m < gpu_n && !buffersSufficiant)) 
    {
	if (Info->Debug) printf("\tDividing Buffer A (k = %lld)\n", k);
#ifdef CALDGEMM_TRANSPOSED_A
	if (divideBuffer(datas[blockm % 2], A + blockm * Info->Height * (TransposeA == CblasTrans ? 1 : A_pitch), Info->Height, Info->Width, BufferHeight, BufferWidth, A_pitch, aPartsNum, TransposeA == CblasNoTrans)) return(1);
#else
	if (divideBuffer(datas[blockm % 2], A + blockm * Info->Height * (TransposeA == CblasTrans ? 1 : A_pitch), Info->Width, Info->Height, BufferWidth, BufferHeight, A_pitch, aPartsNum, TransposeA == CblasTrans)) return(1);
#endif
    }
    if (blockm == 0 || (gpu_m >= gpu_n && !buffersSufficiant))
    {
	if (Info->Debug) printf("\tDividing Buffer B (k = %lld)\n", k);
#ifdef CALDGEMM_TRANSPOSED_B
	divideBuffer(datas[blockn % 2] + aPartsNum, B + blockn * Info->Height * (TransposeB == CblasTrans ? B_pitch : 1), Info->Width, Info->Height, BufferWidth, BufferHeight, B_pitch, bPartsNum, TransposeB == CblasNoTrans);
#else
	divideBuffer(datas[blockn % 2] + aPartsNum, B + blockn * Info->Height * (TransposeB == CblasTrans ? B_pitch : 1), Info->Height, Info->Width, BufferHeight, BufferWidth, B_pitch, bPartsNum, TransposeB == CblasTrans);
#endif
    }
    if (Info->VerboseTiming) Timers.CounterDivide.Stop();

    if (Info->VerboseTiming) Timers.CounterCopyTo.Start();
    if (Info->DivideToGPU == CAL_FALSE)
    {
	if (blockn == 0 || (gpu_m < gpu_n && !buffersSufficiant))
	{
	    if (Info->Debug) printf("\tCopying part of A to GPU (k = %lld)\n", k);
	    if (gpu_m < gpu_n && buffersSufficiant)
	    {
    		if (CopyDataToGPU(&ctx_main, resourceHandlers[j], datas[blockm % 2], aPartsNum, CAL_FALSE, &events[j], datas[blockm] + aPartsNum)) {printf("Error copying to GPU\n"); return(1);}
    	    }
	    else
	    {
    		if (CopyDataToGPU(&ctx_main, resourceHandlers[j], datas[blockm % 2], aPartsNum, CAL_FALSE, &events[j])) {printf("Error copying to GPU\n"); return(1);}
    	    }
    	}
    	
    	if (blockm == 0 || (gpu_m >= gpu_n && !buffersSufficiant))
    	{
    	    if (Info->Debug) printf("\tCopying part of B to GPU (k = %lld)\n", k);
	    if (gpu_m < gpu_n && buffersSufficiant)
	    {
		if (CopyDataToGPU(&ctx_main, resourceHandlers[j] + aPartsNum, datas[blockn % 2] + aPartsNum, bPartsNum, CAL_FALSE, &events[j], datas[blockn % 2])) {printf("Error copying to GPU\n"); return(1);}
	    }
	    else
	    {
    		if (CopyDataToGPU(&ctx_main, resourceHandlers[j] + aPartsNum, datas[blockn % 2] + aPartsNum, bPartsNum, CAL_FALSE, &events[j], datas[nb > bbuffers ? (blockn % 2) : blockn] + aPartsNum)) {printf("Error copying to GPU\n"); return(1);}
    	    }
    	}
    }
    if (Info->VerboseTiming) Timers.CounterCopyTo.Stop();
    calCtxFlush(ctx_main);
}

int caldgemm::ExitCALDGEMM()
{
    if (Info->Debug) printf("Uninitializing CALDGEMM\n");
    for (int i = 0;i < bbuffers;i++)
    {
	//if (Info->Debug) printf("Running Cleanup for Context/bbuffer %d\n", i);
	if (!Cleanup(&device, &ctx_main, modules[i], resourceHandlers[i], datas[i], numInputs + numOutputs + numConstantBuffers, i))
	{
	    return 1;
	}
	if (i < ctxcount && Info->MultiThread)
	{
	    if (Info->Debug) printf("Trying to terminate merge slave %d\n", i);
	    mParam[i].terminate = CAL_TRUE;
	    if (pthread_mutex_unlock(&mParam[i].mergeMutex[1])) printf("Error unlocking mergemutex %d/1 to terminate slave\n", i);
	}
    }
    
    for (int i = 0;i < 1;i++) for (int j = 0;j < kernel_count;j++) delete[] progNames[i][j];
    
    if (Info->UseCPU && Info->UseGPU)
    {
	if (Info->Debug) printf("Trying to terminate blas slave\n");
	cParam.terminate = CAL_TRUE;
        if (pthread_mutex_unlock(&cParam.cblasMutex[1])) printf("Error unlocking blas mutex 1 to terminate thread\n");
        if (pthread_mutex_unlock(&cParam.cblasMutex[0])) printf("Error unlocking blas mutex 0 to terminate thread\n");
    }
    
    if (Info->MultiThread)
    {
	for (int i = 0;i < ctxcount;i++)
	{
	    while (pthread_mutex_trylock(&mParam[i].mergeMutex[1]) != EBUSY) pthread_mutex_unlock(&mParam[i].mergeMutex[1]);
	    if (pthread_mutex_unlock(&mParam[i].mergeMutex[1])) printf("Error unlocking mergeMutex %d/1\n", i);
	}
	if (Info->UseCPU && Info->UseGPU)
	{
    	    while (pthread_mutex_trylock(&cParam.cblasMutex[1]) != EBUSY) pthread_mutex_unlock(&cParam.cblasMutex[1]);
	    if (pthread_mutex_unlock(&cParam.cblasMutex[1])) printf("Error unlocking blasMutex 1\n");
	}
    }
    
    for (int j = 0;j < 2;j++)
    {
	if (Info->MultiThread) for (int i = 0;i < ctxcount;i++) if (pthread_mutex_destroy(&mParam[i].mergeMutex[j])) printf("Error destroying mergemutex %d/%d\n", i, j);
	if (Info->UseCPU && Info->UseGPU) if (pthread_mutex_destroy(&cParam.cblasMutex[j])) printf("Error destroying blas mutex %d\n", j);
    }

    // Close the device
    if (ctx_main) calCtxDestroy(ctx_main);
    if (device)
    {
        if (calDeviceClose(device) != CAL_RESULT_OK )
        {
            fprintf(stderr, "There was an error closing the device.\n");
            fprintf(stderr, "Error string is %s\n", calGetErrorString());
        }
    }

    // Shutdown cal device
    if (calShutdown() != CAL_RESULT_OK )
    {
        fprintf(stderr, "There was an error during cal shutdown.\n");
        fprintf(stderr, "Error string is %s\n", calGetErrorString());
    }

    return(0);
}

void caldgemm::ResetTimers()
{
    //Reset Timers
    Timers.System.Reset();
    Timers.Kernel.Reset();
    Timers.CounterDivide.Reset();
    Timers.CounterMerge.Reset();
    Timers.CounterCopyTo.Reset();
    Timers.CounterCopyFrom.Reset();
    Timers.CPUTimer.Reset();
    Timers.GPUTimer.Reset();
}
