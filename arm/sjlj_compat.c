/*
 * Stubs for old ARM C++ binaries compiled with SJ (setjmp/longjmp)
 * exception handling.
 *
 * Modern ARM toolchains use DWARF-based unwinding. These old binaries
 * reference SJLJ symbols that no longer exist in modern libstdc++/libgcc_s.
 *
 * libeci.so is a C-API library -- C++ exceptions never cross the API
 * boundary, so these routines should never actually be invoked at runtime.
 * The symbols just need to exist to satisfy the dynamic linker.
 */
#include <stdlib.h>

/* From CXXABI_1.3 -- C++ personality routine for SJLJ exception handling */
__attribute__((visibility("default")))
int __gxx_personality_sj0(int version, ...)
{
    abort();
    return 0;
}

/* From GCC_3.0 -- SJLJ unwinding primitives */
__attribute__((visibility("default")))
void _Unwind_SjLj_Register(void *context)
{
    (void)context;
}

__attribute__((visibility("default")))
void _Unwind_SjLj_Unregister(void *context)
{
    (void)context;
}

__attribute__((visibility("default")))
void _Unwind_SjLj_Resume(void *exception)
{
    (void)exception;
    abort();
}
