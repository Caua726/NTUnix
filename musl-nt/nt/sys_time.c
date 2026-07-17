#include "nt/ntpriv.h"

static void precise_system_time(FILETIME *ft)
{
    typedef VOID (WINAPI *precise_fn)(LPFILETIME);
    static precise_fn precise;
    if (!precise) {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel)
            precise = (precise_fn)(void *)GetProcAddress(
                kernel, "GetSystemTimePreciseAsFileTime");
    }
    if (precise) precise(ft);
    else GetSystemTimeAsFileTime(ft);
}

nt_sc_t nt_sys_clock_gettime(nt_sc_t clockid, nt_sc_t ts_arg)
{
    struct nt_timespec *ts = (void *)(uintptr_t)ts_arg;
    if (!ts) return -NT_EFAULT;
    if (clockid == NT_CLOCK_REALTIME) {
        FILETIME ft;
        precise_system_time(&ft);
        nt_filetime_to_timespec(ft, ts);
        return 0;
    }
    if (clockid == NT_CLOCK_MONOTONIC || clockid == 4 || clockid == 7) {
        LARGE_INTEGER counter, frequency;
        uint64_t seconds, remainder;
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        seconds = (uint64_t)counter.QuadPart / (uint64_t)frequency.QuadPart;
        remainder = (uint64_t)counter.QuadPart % (uint64_t)frequency.QuadPart;
        ts->tv_sec = (int64_t)seconds;
        ts->tv_nsec = (int64_t)((remainder * 1000000000ULL) /
                                (uint64_t)frequency.QuadPart);
        return 0;
    }
    if (clockid == NT_CLOCK_PROCESS_CPUTIME_ID ||
        clockid == NT_CLOCK_THREAD_CPUTIME_ID) {
        FILETIME create, exit, kernel, user;
        ULARGE_INTEGER k, u;
        BOOL ok = clockid == NT_CLOCK_PROCESS_CPUTIME_ID
                    ? GetProcessTimes(GetCurrentProcess(), &create, &exit,
                                      &kernel, &user)
                    : GetThreadTimes(GetCurrentThread(), &create, &exit,
                                     &kernel, &user);
        if (!ok) return nt_last_error();
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        k.QuadPart += u.QuadPart;
        ts->tv_sec = (int64_t)(k.QuadPart / 10000000ULL);
        ts->tv_nsec = (int64_t)((k.QuadPart % 10000000ULL) * 100ULL);
        return 0;
    }
    return -NT_EINVAL;
}

nt_sc_t nt_sys_clock_getres(nt_sc_t clockid, nt_sc_t ts_arg)
{
    struct nt_timespec *ts = (void *)(uintptr_t)ts_arg;
    if (!ts) return 0;
    if (clockid == NT_CLOCK_MONOTONIC || clockid == 4 || clockid == 7) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        ts->tv_sec = 0;
        ts->tv_nsec = 1000000000LL / frequency.QuadPart;
        if (!ts->tv_nsec) ts->tv_nsec = 1;
        return 0;
    }
    if (clockid == NT_CLOCK_REALTIME ||
        clockid == NT_CLOCK_PROCESS_CPUTIME_ID ||
        clockid == NT_CLOCK_THREAD_CPUTIME_ID) {
        ts->tv_sec = 0;
        ts->tv_nsec = 100;
        return 0;
    }
    return -NT_EINVAL;
}

nt_sc_t nt_sys_clock_settime(nt_sc_t clockid, nt_sc_t ts_arg)
{
    const struct nt_timespec *ts = (const void *)(uintptr_t)ts_arg;
    FILETIME ft;
    SYSTEMTIME system;
    if (clockid != NT_CLOCK_REALTIME || !ts) return -NT_EINVAL;
    if (ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000) return -NT_EINVAL;
    ft = nt_timespec_to_filetime(ts);
    if (!FileTimeToSystemTime(&ft, &system)) return nt_last_error();
    if (!SetSystemTime(&system)) return nt_last_error();
    return 0;
}

nt_sc_t nt_sys_clock_nanosleep(nt_sc_t clockid, nt_sc_t flags, nt_sc_t req_arg, nt_sc_t rem_arg)
{
    const struct nt_timespec *req = (const void *)(uintptr_t)req_arg;
    struct nt_timespec *rem = (void *)(uintptr_t)rem_arg;
    struct nt_timespec delta;
    HANDLE timer;
    LARGE_INTEGER due;
    int64_t ticks;
    if (!req) return -NT_EFAULT;
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000 || req->tv_sec < 0)
        return -NT_EINVAL;
    delta = *req;
    if (flags & NT_TIMER_ABSTIME) {
        struct nt_timespec now;
        nt_sc_t r = nt_sys_clock_gettime(clockid, (nt_sc_t)(uintptr_t)&now);
        if (r < 0) return r;
        delta.tv_sec -= now.tv_sec;
        delta.tv_nsec -= now.tv_nsec;
        if (delta.tv_nsec < 0) {
            --delta.tv_sec;
            delta.tv_nsec += 1000000000;
        }
        if (delta.tv_sec < 0) return 0;
    } else if (flags) {
        return -NT_EINVAL;
    }
    if (clockid != NT_CLOCK_REALTIME && clockid != NT_CLOCK_MONOTONIC &&
        clockid != 4 && clockid != 7)
        return -NT_EINVAL;
    ticks = delta.tv_sec * 10000000LL + delta.tv_nsec / 100;
    if (!ticks) {
        SwitchToThread();
        return 0;
    }
    timer = CreateWaitableTimerW(0, TRUE, 0);
    if (!timer) return nt_last_error();
    due.QuadPart = -ticks;
    if (!SetWaitableTimer(timer, &due, 0, 0, 0, FALSE)) {
        nt_sc_t r = nt_last_error();
        CloseHandle(timer);
        return r;
    }
    WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}

nt_sc_t nt_sys_gettimeofday(nt_sc_t tv_arg, nt_sc_t tz_arg)
{
    struct nt_timeval *tv = (void *)(uintptr_t)tv_arg;
    struct nt_timezone *tz = (void *)(uintptr_t)tz_arg;
    struct nt_timespec ts;
    nt_sc_t r = nt_sys_clock_gettime(NT_CLOCK_REALTIME, (nt_sc_t)(uintptr_t)&ts);
    if (r < 0) return r;
    if (tv) {
        tv->tv_sec = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
    }
    if (tz) tz->tz_minuteswest = tz->tz_dsttime = 0;
    return 0;
}
