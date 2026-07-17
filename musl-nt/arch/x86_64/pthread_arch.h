#include <stdint.h>

/* PE has no ELF TLS contract.  Keep musl's pthread object unchanged and put
 * its thread pointer in a native NT TLS slot.  All x86-64 musl accesses are
 * funneled through __get_tp(), so this avoids claiming %fs (which also makes
 * hosted tests under Wine safe). */
uintptr_t __nt_get_tp(void);

static inline uintptr_t __get_tp(void)
{
    return __nt_get_tp();
}

#define MC_PC gregs[REG_RIP]
