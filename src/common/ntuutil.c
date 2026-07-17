/* ntuutil.c — utilidades pequenas compartilhadas. */
#include "ntu.h"

void ntu_trim(char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
}

void ntu_now(char *out, size_t cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}
