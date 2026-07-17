#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>
#include <stdint.h>

extern int __attribute__((sysv_abi)) main(int, char **, char **);
extern int __attribute__((sysv_abi))
__libc_start_main(int (__attribute__((sysv_abi)) *)(int, char **, char **),
                  int, char **,
                  void (*)(), void (*)(), void (*)());

/* LLVM's Windows lowering inserts this hook at the start of main.  musl runs
 * initializers from __libc_start_main, so the MinGW libgcc hook must be inert. */
void __main(void) {}

static size_t wide_length(const WCHAR *s)
{
    const WCHAR *p = s;
    while (*p) ++p;
    return (size_t)(p - s);
}

static void *align_pointer(void *p)
{
    uintptr_t value = (uintptr_t)p;
    value = (value + sizeof(void *) - 1) & ~(uintptr_t)(sizeof(void *) - 1);
    return (void *)value;
}

/* Parse according to the backslash/quote rules used by the Windows CRT, but
 * without linking that CRT.  The input is copied and tokenized in place. */
static int split_command_line(const WCHAR *input, WCHAR *copy, size_t copy_cap,
                              WCHAR **args, int args_cap)
{
    const WCHAR *p = input;
    WCHAR *out = copy;
    WCHAR *end = copy + copy_cap;
    int argc = 0;
    while (*p) {
        int quoted = 0;
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;
        if (argc >= args_cap || out >= end) return -1;
        args[argc++] = out;
        for (;;) {
            size_t slashes = 0, i;
            while (*p == L'\\') { ++slashes; ++p; }
            if (*p == L'"') {
                for (i = 0; i < slashes / 2; ++i) {
                    if (out >= end) return -1;
                    *out++ = L'\\';
                }
                if (slashes & 1) {
                    if (out >= end) return -1;
                    *out++ = L'"';
                    ++p;
                } else {
                    quoted = !quoted;
                    ++p;
                }
                continue;
            }
            for (i = 0; i < slashes; ++i) {
                if (out >= end) return -1;
                *out++ = L'\\';
            }
            if (!*p || (!quoted && (*p == L' ' || *p == L'\t'))) break;
            if (out >= end) return -1;
            *out++ = *p++;
        }
        if (out >= end) return -1;
        *out++ = 0;
        while (*p == L' ' || *p == L'\t') ++p;
    }
    return argc;
}

static size_t environment_count(const WCHAR *block)
{
    size_t count = 0;
    while (*block) {
        ++count;
        block += wide_length(block) + 1;
    }
    return count;
}

static size_t utf8_bytes(const WCHAR *s)
{
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, -1,
                                0, 0, 0, 0);
    return n > 0 ? (size_t)n : 0;
}

static int convert_utf8(const WCHAR *src, char *dst, size_t cap)
{
    if (cap > 0x7fffffffU) return 0;
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1,
                               dst, (int)cap, 0, 0) != 0;
}

static void normalize_program_name(char *name)
{
    /* Windows supplies argv[0] with backslashes.  POSIX consumers such as
     * BusyBox find their applet name with strrchr(argv[0], '/'). */
    while (*name) {
        if (*name == '\\') *name = '/';
        ++name;
    }
}

static void fill_random(unsigned char *out, size_t length)
{
    typedef BOOLEAN (WINAPI *random_fn)(PVOID, ULONG);
    HMODULE advapi = LoadLibraryW(L"advapi32.dll");
    random_fn fn = advapi ? (random_fn)(void *)
        GetProcAddress(advapi, "SystemFunction036") : 0;
    if (fn && fn(out, (ULONG)length)) return;
    {
        LARGE_INTEGER qpc;
        size_t i;
        uint64_t state;
        QueryPerformanceCounter(&qpc);
        state = (uint64_t)qpc.QuadPart ^ (uintptr_t)out ^
                ((uint64_t)GetCurrentProcessId() << 32);
        for (i = 0; i < length; ++i) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            out[i] = (unsigned char)state;
        }
    }
}

__attribute__((noreturn)) void mainCRTStartup(void)
{
    const WCHAR *command = GetCommandLineW();
    WCHAR *environment = GetEnvironmentStringsW();
    size_t command_len = wide_length(command);
    size_t max_args = command_len / 2 + 2;
    WCHAR *command_copy;
    WCHAR **wide_argv;
    int argc, i;
    size_t envc, string_bytes = 0;
    const WCHAR *ep;
    size_t pointer_count, aux_pairs = 9;
    size_t allocation_size;
    unsigned char *allocation, *cursor;
    char **argv, **envp;
    size_t *auxv;
    unsigned char *random;
    int result;

    command_copy = VirtualAlloc(0, (command_len + 1) * sizeof(WCHAR),
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    wide_argv = VirtualAlloc(0, max_args * sizeof(WCHAR *),
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!command_copy || !wide_argv || !environment) ExitProcess(127);
    argc = split_command_line(command, command_copy, command_len + 1,
                              wide_argv, (int)max_args);
    if (argc < 0) ExitProcess(127);
    envc = environment_count(environment);
    for (i = 0; i < argc; ++i) string_bytes += utf8_bytes(wide_argv[i]);
    for (ep = environment; *ep; ep += wide_length(ep) + 1)
        string_bytes += utf8_bytes(ep);
    pointer_count = (size_t)argc + 1 + envc + 1;
    allocation_size = pointer_count * sizeof(char *) +
                      aux_pairs * 2 * sizeof(size_t) + 32 + string_bytes;
    allocation = VirtualAlloc(0, allocation_size, MEM_RESERVE | MEM_COMMIT,
                              PAGE_READWRITE);
    if (!allocation) ExitProcess(127);
    argv = (char **)allocation;
    envp = argv + argc + 1;
    auxv = (size_t *)(envp + envc + 1);
    random = align_pointer(auxv + aux_pairs * 2);
    cursor = random + 16;

    for (i = 0; i < argc; ++i) {
        size_t n = utf8_bytes(wide_argv[i]);
        argv[i] = (char *)cursor;
        if (!convert_utf8(wide_argv[i], (char *)cursor, n)) ExitProcess(127);
        cursor += n;
    }
    argv[argc] = 0;
    if (argc) normalize_program_name(argv[0]);
    i = 0;
    for (ep = environment; *ep; ep += wide_length(ep) + 1) {
        size_t n = utf8_bytes(ep);
        envp[i++] = (char *)cursor;
        if (!convert_utf8(ep, (char *)cursor, n)) ExitProcess(127);
        cursor += n;
    }
    envp[i] = 0;
    fill_random(random, 16);

    /* Minimal Linux-shaped auxv consumed by musl's __init_libc. */
    i = 0;
#define AUX(key, value) do { auxv[i++] = (key); auxv[i++] = (size_t)(value); } while (0)
    AUX(6, 4096);                  /* AT_PAGESZ */
    AUX(11, 0); AUX(12, 0);       /* AT_UID / AT_EUID */
    AUX(13, 0); AUX(14, 0);       /* AT_GID / AT_EGID */
    AUX(23, 0);                    /* AT_SECURE */
    AUX(25, random);               /* AT_RANDOM */
    AUX(31, argc ? argv[0] : 0);   /* AT_EXECFN */
    AUX(0, 0);                     /* AT_NULL */
#undef AUX

    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    result = __libc_start_main(main, argc, argv, 0, 0, 0);
    ExitProcess((UINT)result);
}
