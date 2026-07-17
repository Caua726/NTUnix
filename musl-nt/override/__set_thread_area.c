#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>

static INIT_ONCE tls_once = INIT_ONCE_STATIC_INIT;
static DWORD tls_index = TLS_OUT_OF_INDEXES;

static BOOL CALLBACK initialize_tls_index(PINIT_ONCE once, PVOID parameter,
                                          PVOID *context)
{
    (void)once; (void)parameter; (void)context;
    tls_index = TlsAlloc();
    return TRUE;
}

static int ensure_tls_index(void)
{
    InitOnceExecuteOnce(&tls_once, initialize_tls_index, 0, 0);
    return tls_index != TLS_OUT_OF_INDEXES;
}

__attribute__((sysv_abi)) int __set_thread_area(void *pointer)
{
    return ensure_tls_index() && TlsSetValue(tls_index, pointer) ? 0 : -1;
}

__attribute__((sysv_abi)) uintptr_t __nt_get_tp(void)
{
    if (!ensure_tls_index()) return 0;
    return (uintptr_t)TlsGetValue(tls_index);
}
