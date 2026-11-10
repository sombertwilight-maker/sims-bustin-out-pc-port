# Cross-compile with clang for the MinGW-w64 x86-64 target.
#
# Why clang and not mingw gcc: the recompiled code keeps the guest's registers
# in thread-local variables and touches them on almost every line. GCC on
# MinGW implements __thread through emutls (a function call per access);
# clang emits native Windows TLS (gs:[0x58]), the same code MSVC produces.
#
#     cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Release \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/clang-mingw-x86_64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLET x86_64-w64-mingw32)
set(MINGW_SYSROOT /usr/${MINGW_TRIPLET})
if(NOT DEFINED MINGW_GCC_LIBDIR)
    file(GLOB _gccdirs "/usr/lib/gcc/${MINGW_TRIPLET}/*-win32")
    list(GET _gccdirs 0 MINGW_GCC_LIBDIR)
endif()

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${MINGW_TRIPLET})
set(CMAKE_CXX_COMPILER_TARGET ${MINGW_TRIPLET})
set(CMAKE_RC_COMPILER ${MINGW_TRIPLET}-windres)
set(CMAKE_AR ${MINGW_TRIPLET}-ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${MINGW_TRIPLET}-ranlib CACHE FILEPATH "")

set(CMAKE_C_FLAGS_INIT "--sysroot=${MINGW_SYSROOT}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--sysroot=${MINGW_SYSROOT} -L${MINGW_GCC_LIBDIR} -static")

set(CMAKE_FIND_ROOT_PATH ${MINGW_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Lets the toolkit's `$<BOOL:${MINGW}>` link choices (xinput1_4) apply.
set(MINGW 1)
