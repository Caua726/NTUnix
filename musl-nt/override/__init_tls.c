#define SYSCALL_NO_TLS 1
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "pthread_impl.h"
#include "libc.h"
#include "atomic.h"
#include "syscall.h"

volatile int __thread_list_lock;

int __init_tp(void *p)
{
    pthread_t td = p;
    td->self = td;
    if (__set_thread_area(TP_ADJ(p)) < 0) return -1;
    libc.can_do_threads = 1;
    td->detach_state = DT_JOINABLE;
    td->tid = __syscall(SYS_set_tid_address, &__thread_list_lock);
    td->locale = &libc.global_locale;
    td->robust_list.head = &td->robust_list.head;
    td->next = td->prev = td;
    return 0;
}

static struct builtin_tls {
    char alignment;
    struct pthread pthread;
    void *space[16];
} builtin_tls[1];

#define MIN_TLS_ALIGN offsetof(struct builtin_tls, pthread)

void *__copy_tls(unsigned char *mem)
{
    uintptr_t *dtv = (uintptr_t *)mem;
    pthread_t td;
    mem += libc.tls_size - sizeof(struct pthread);
    mem -= (uintptr_t)mem & (libc.tls_align - 1);
    td = (pthread_t)mem;
    memset(td, 0, sizeof *td);
    dtv[0] = 0;
    td->dtv = dtv;
    return td;
}

void __init_tls(size_t *aux)
{
    void *mem;
    (void)aux;
    libc.tls_cnt = 0;
    libc.tls_head = 0;
    libc.tls_align = MIN_TLS_ALIGN;
    if (libc.tls_align < sizeof(void *)) libc.tls_align = sizeof(void *);
    libc.tls_size = 2 * sizeof(void *) + sizeof(struct pthread) +
                    libc.tls_align + MIN_TLS_ALIGN - 1;
    if (libc.tls_size <= sizeof builtin_tls) {
        mem = builtin_tls;
        memset(mem, 0, sizeof builtin_tls);
    } else {
        mem = (void *)__syscall(SYS_mmap, 0, libc.tls_size,
                                PROT_READ | PROT_WRITE,
                                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    }
    if (__init_tp(__copy_tls(mem)) < 0) a_crash();
}
