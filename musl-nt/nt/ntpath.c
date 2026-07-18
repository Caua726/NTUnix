#include "nt/ntpriv.h"

static WCHAR nt_root[NT_PATH_CAP];
static INIT_ONCE root_once = INIT_ONCE_STATIC_INIT;

size_t nt_strlen(const char *s)
{
    const char *p = s;
    if (!s) return 0;
    while (*p) ++p;
    return (size_t)(p - s);
}

size_t nt_wcslen(const WCHAR *s)
{
    const WCHAR *p = s;
    if (!s) return 0;
    while (*p) ++p;
    return (size_t)(p - s);
}

void *nt_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *nt_memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)value;
    return dst;
}

int nt_streq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

nt_sc_t nt_utf8_to_wide(const char *src, WCHAR *dst, size_t cap)
{
    int count;
    if (!src || !dst || !cap) return -NT_EFAULT;
    if (cap > 0x7fffffffU) return -NT_ENAMETOOLONG;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst,
                                (int)cap);
    if (!count) {
        DWORD e = GetLastError();
        return e == ERROR_INSUFFICIENT_BUFFER ? -NT_ENAMETOOLONG : nt_error(e);
    }
    return count - 1;
}

nt_sc_t nt_wide_to_utf8(const WCHAR *src, size_t len, char *dst, size_t cap,
                     size_t *written)
{
    int count;
    if (!src || !dst || !cap) return -NT_EFAULT;
    if (len > 0x7fffffffU || cap > 0x7fffffffU) return -NT_ENAMETOOLONG;
    count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, (int)len,
                                dst, (int)(cap - 1), 0, 0);
    if (!count && len) {
        DWORD e = GetLastError();
        return e == ERROR_INSUFFICIENT_BUFFER ? -NT_ENAMETOOLONG : nt_error(e);
    }
    dst[count] = 0;
    if (written) *written = (size_t)count;
    return count;
}

static void strip_filename(WCHAR *path)
{
    size_t n = nt_wcslen(path);
    while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
    if (n) path[n - 1] = 0;
}

static int suffix_equal_ci(const WCHAR *s, const WCHAR *suffix)
{
    size_t ns = nt_wcslen(s), nx = nt_wcslen(suffix), i;
    if (nx > ns) return 0;
    s += ns - nx;
    for (i = 0; i < nx; ++i) {
        WCHAR a = s[i], b = suffix[i];
        if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
        if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
        if (a != b) return 0;
    }
    return 1;
}

static BOOL CALLBACK initialize_root(PINIT_ONCE once, PVOID parameter,
                                     PVOID *context)
{
    DWORD n;
    (void)once;
    (void)parameter;
    (void)context;
    n = GetEnvironmentVariableW(L"NTUNIX_ROOT", nt_root, NT_PATH_CAP);
    if (!n || n >= NT_PATH_CAP) {
        n = GetModuleFileNameW(0, nt_root, NT_PATH_CAP);
        if (!n || n >= NT_PATH_CAP) {
            n = GetCurrentDirectoryW(NT_PATH_CAP, nt_root);
            if (!n || n >= NT_PATH_CAP) nt_root[0] = 0;
        } else {
            strip_filename(nt_root);
            if (suffix_equal_ci(nt_root, L"\\system\\bin")) {
                size_t len = nt_wcslen(nt_root) - nt_wcslen(L"\\system\\bin");
                nt_root[len] = 0;
            }
        }
    }
    n = (DWORD)nt_wcslen(nt_root);
    while (n > 3 && (nt_root[n - 1] == L'\\' || nt_root[n - 1] == L'/'))
        nt_root[--n] = 0;
    return TRUE;
}

static nt_sc_t copy_wide(WCHAR *dst, size_t cap, const WCHAR *src)
{
    size_t n = nt_wcslen(src);
    if (n + 1 > cap) return -NT_ENAMETOOLONG;
    nt_memcpy(dst, src, (n + 1) * sizeof(WCHAR));
    return (nt_sc_t)n;
}

static nt_sc_t base_for_dirfd(int dirfd, WCHAR *out, size_t cap)
{
    DWORD n;
    if (dirfd == NT_AT_FDCWD) {
        n = GetCurrentDirectoryW((DWORD)cap, out);
        if (!n) return nt_last_error();
        if (n >= cap) return -NT_ENAMETOOLONG;
        return n;
    }
    {
        struct nt_fd *slot = nt_fd_get(dirfd);
        if (!slot) return -NT_EBADF;
        n = GetFinalPathNameByHandleW(slot->handle, out, (DWORD)cap,
                                      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!n) return nt_last_error();
        if (n >= cap) return -NT_ENAMETOOLONG;
        if (n >= 4 && out[0] == L'\\' && out[1] == L'\\' && out[2] == L'?' &&
            out[3] == L'\\') {
            size_t i;
            for (i = 4; i <= n; ++i) out[i - 4] = out[i];
            n -= 4;
        }
        return n;
    }
}

nt_sc_t nt_path_at(int dirfd, const char *path, WCHAR *out, size_t cap)
{
    WCHAR converted[NT_PATH_CAP];
    size_t prefix, suffix, i;
    nt_sc_t r;
    int win_absolute;
    if (!path || !out) return -NT_EFAULT;
    if (!*path) return -NT_ENOENT;
    if (cap > NT_PATH_CAP) cap = NT_PATH_CAP;
    /* /proc/self/exe: o BusyBox (build sem-MMU) re-exec'a a si mesmo por este
     * caminho a cada fork+exec de applet. Como a NTUnix não tem /proc real,
     * resolve para o binário do processo atual; senão o CreateProcessW do
     * execve falha e todo applet exec'ado (cat, wc, sort, grep…) sai mudo. */
    if (nt_streq(path, "/proc/self/exe") || nt_streq(path, "/proc/self/exe/")) {
        DWORD n = GetModuleFileNameW(0, out, (DWORD)cap);
        if (!n || n >= cap) return -NT_ENAMETOOLONG;
        return (nt_sc_t)n;
    }
    r = nt_utf8_to_wide(path, converted, NT_ARRAY_LEN(converted));
    if (r < 0) return r;
    suffix = (size_t)r;
    win_absolute = ((converted[0] && converted[1] == L':') ||
                    (converted[0] == L'\\' && converted[1] == L'\\'));
    if (win_absolute) {
        r = copy_wide(out, cap, converted);
        if (r < 0) return r;
    } else if (converted[0] == L'/' || converted[0] == L'\\') {
        /* /mnt/<letra>[/...] -> <LETRA>:\... (drives do Windows, estilo WSL).
         * Ex.: /mnt/x/windows/system32 -> X:\windows\system32. Deixa o ash sair
         * da raiz do NTUnix e alcançar X:\, C:\ etc. */
        WCHAR d = (converted[1] == L'm' && converted[2] == L'n' &&
                   converted[3] == L't' && converted[4] == L'/') ? converted[5] : 0;
        if (((d >= L'a' && d <= L'z') || (d >= L'A' && d <= L'Z')) &&
            (converted[6] == L'/' || converted[6] == 0)) {
            WCHAR drive = (d >= L'a' && d <= L'z') ? (WCHAR)(d - 32) : d;
            const WCHAR *rest = converted + 6;    /* aponta pro '/' ou terminador */
            size_t rl = nt_wcslen(rest);
            if (2 + (rl ? rl : 1) + 1 > cap) return -NT_ENAMETOOLONG;
            out[0] = drive;
            out[1] = L':';
            if (!rl) { out[2] = L'\\'; out[3] = 0; r = 3; }
            else { nt_memcpy(out + 2, rest, (rl + 1) * sizeof(WCHAR)); r = (nt_sc_t)(2 + rl); }
        } else {
            InitOnceExecuteOnce(&root_once, initialize_root, 0, 0);
            prefix = nt_wcslen(nt_root);
            if (prefix + suffix + 1 > cap) return -NT_ENAMETOOLONG;
            nt_memcpy(out, nt_root, prefix * sizeof(WCHAR));
            nt_memcpy(out + prefix, converted, (suffix + 1) * sizeof(WCHAR));
            r = (nt_sc_t)(prefix + suffix);
        }
    } else {
        r = base_for_dirfd(dirfd, out, cap);
        if (r < 0) return r;
        prefix = (size_t)r;
        if (prefix + 1 + suffix + 1 > cap) return -NT_ENAMETOOLONG;
        if (prefix && out[prefix - 1] != L'\\') out[prefix++] = L'\\';
        nt_memcpy(out + prefix, converted, (suffix + 1) * sizeof(WCHAR));
        r = (nt_sc_t)(prefix + suffix);
    }
    for (i = 0; out[i]; ++i)
        if (out[i] == L'/') out[i] = L'\\';
    return r;
}

nt_sc_t nt_path_to_unix(const WCHAR *path, char *out, size_t cap)
{
    size_t n, i;
    nt_sc_t r;
    if (!path || !out) return -NT_EFAULT;
    /* Representacao canonica: TUDO sob /mnt/<letra>/. Um caminho com drive
     * (X:\...) vira /mnt/x/... — inclusive a raiz do NTUnix (X:\NTUnix ->
     * /mnt/x/NTUnix). Assim o getcwd nunca dessincroniza (o CWD real sempre tem
     * um caminho unix valido), a navegacao e' consistente e drives diferentes
     * (C:\, D:\) aparecem naturalmente. O forward mapping ainda aceita /system/..
     * (via nt_root) e /mnt/x/NTUnix/system/.. — os dois chegam no mesmo lugar. */
    if (((path[0] >= L'A' && path[0] <= L'Z') ||
         (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':') {
        WCHAR d = path[0];
        if (d >= L'A' && d <= L'Z') d += 32;
        if (6 >= cap) return -NT_ERANGE;
        out[0] = '/'; out[1] = 'm'; out[2] = 'n'; out[3] = 't';
        out[4] = '/'; out[5] = (char)d;
        r = nt_wide_to_utf8(path + 2, nt_wcslen(path + 2), out + 6, cap - 6, &n);
        if (r < 0) return r;
        n += 6;
        for (i = 6; i < n; ++i) if (out[i] == '\\') out[i] = '/';
        if (n > 6 && out[n - 1] == '/') n--;   /* X:\ -> /mnt/x (sem barra final) */
        if (n < cap) out[n] = 0;
        return (nt_sc_t)(n + 1);
    }
    /* UNC (\\host\share) e outros: converte cru */
    r = nt_wide_to_utf8(path, nt_wcslen(path), out, cap, &n);
    if (r < 0) return r;
    for (i = 0; i < n; ++i) if (out[i] == '\\') out[i] = '/';
    return (nt_sc_t)(n + 1); /* getcwd returns byte count including NUL */
}
