INTELARCH					= SSE4.2
CUDAVERSION					= 20
CUDAREGS					= 64
ARCHBITS					= 64

HIDEECHO					= @
CC_x86_64-pc-linux-gnu		= GCC
CC_i686-pc-cygwin			= ICC

TARGET						= dgemm_template

INTELFLAGSUSE				= $(INTELFLAGSOPT)
VSNETFLAGSUSE				= $(VSNETFLAGSOPT)
GCCFLAGSUSE					= $(GCCFLAGSOPT)
NVCCFLAGSUSE				= $(NVCCFLAGSOPT)

TARGETTYPE					= LIB

CPPFILES					= template.cpp

CONFIG_OPENCL				= 1