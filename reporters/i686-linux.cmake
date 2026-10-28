# Run with an x86 GNU-compatible multilib compiler and 32-bit libc/libstdc++.
# Do not replace host compilers or change the main application's architecture.
set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_CXX_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
