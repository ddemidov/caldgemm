const char *caldgemm_opencl::OCLKernelName =
OCL_KERNEL_PRE
"union double_read {uint4 f; double2 d;};\n"
"__kernel void oclkernel(__global double* C, image2d_t A, image2d_t B, int height1, int height2, int width, double alpha, double beta)\n"
"{\n"
"	int i, j, k;\n"
"	for (i = get_global_id(1);i < height2;i += get_global_size(1))\n"
"	{\n"
"		for (j = get_global_id(0);j < height1;j += get_global_size(0))\n"
"		{\n"
"			double addval = 0.;\n"
"			for (k = 0;k < width / 2;k++)\n"
"			{\n"
"				float2 coord;\n"
"				union double_read tmp, tmp2;\n"
"				coord.x = k;\n"
"				coord.y = i;\n"
"				tmp.f = read_imageui(A, sampler, coord);\n"
"				coord.y = j;\n"
"				tmp2.f = read_imageui(B, sampler, coord);\n"
"				addval += tmp.d.x * tmp2.d.x + tmp.d.y * tmp2.d.y;\n"
"			}\n"
"			C[i * height1 + j] = beta * C[i * height1 + j] + alpha * addval;\n"
"		}\n"
"	}\n"
"}\n"
;
