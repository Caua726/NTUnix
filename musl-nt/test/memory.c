#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern void *sbrk(intptr_t);

int main(void)
{
    void *initial_break = sbrk(0);
    void *mapping = mmap(0, 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *small = malloc(128);
    void *zeroed = calloc(4, 32);
    int ok = initial_break != (void *)-1 && mapping != MAP_FAILED &&
             small != 0 && zeroed != 0;
    size_t i;

    if (mapping != MAP_FAILED) {
        memset(mapping, 0xa5, 4096);
        ok &= ((unsigned char *)mapping)[0] == 0xa5 &&
              ((unsigned char *)mapping)[4095] == 0xa5;
    }
    if (small) {
        memset(small, 0x5a, 128);
        ok &= ((unsigned char *)small)[127] == 0x5a;
    }
    if (zeroed)
        for (i = 0; i < 4 * 32; ++i)
            if (((unsigned char *)zeroed)[i]) ok = 0;

    printf("brk=%p mmap=%p malloc=%p calloc=%p result=%s\n",
           initial_break, mapping, small, zeroed, ok ? "ok" : "FAIL");
    if (mapping != MAP_FAILED) munmap(mapping, 4096);
    free(small);
    free(zeroed);
    return !ok;
}
