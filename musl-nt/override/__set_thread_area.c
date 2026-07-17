#include <stdint.h>

/* Thread pointer da musl.  A NTUnix é single-thread (não há SYS_clone para
 * threads; pthread_create -> ENOSYS), então "por thread" == global.  Guardar o
 * TP num GLOBAL (em vez de um slot TLS do TEB) é o que faz o fork estilo-Cygwin
 * funcionar: a cópia de memória do processo já leva o TP junto, sem precisar
 * mexer no TEB do filho recém-criado. */
static void *g_thread_pointer;

__attribute__((sysv_abi)) int __set_thread_area(void *pointer)
{
    g_thread_pointer = pointer;
    return 0;
}

__attribute__((sysv_abi)) uintptr_t __nt_get_tp(void)
{
    return (uintptr_t)g_thread_pointer;
}
