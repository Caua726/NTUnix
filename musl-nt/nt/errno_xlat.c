#include "nt/ntpriv.h"

int nt_errno_from_win32(DWORD e)
{
    switch (e) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return NT_ENOENT;
    case ERROR_TOO_MANY_OPEN_FILES: return NT_EMFILE;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_CANNOT_MAKE:
        return NT_EACCES;
    case ERROR_INVALID_HANDLE: return NT_EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT:
        return NT_ENOMEM;
    case ERROR_INVALID_DATA:
    case ERROR_INVALID_PARAMETER:
    case ERROR_NEGATIVE_SEEK:
    case ERROR_NOT_A_REPARSE_POINT:
        return NT_EINVAL;
    case ERROR_NOT_SAME_DEVICE: return NT_EXDEV;
    case ERROR_NO_MORE_FILES: return NT_ENOENT;
    case ERROR_WRITE_PROTECT: return NT_EROFS;
    case ERROR_NOT_READY:
    case ERROR_DEV_NOT_EXIST:
        return NT_ENODEV;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_BUSY:
    case ERROR_PIPE_BUSY:
        return NT_EBUSY;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return NT_EEXIST;
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE:
        return NT_ENAMETOOLONG;
    case ERROR_DIR_NOT_EMPTY: return NT_ENOTEMPTY;
    case ERROR_DIRECTORY: return NT_ENOTDIR;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return NT_ENOSPC;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
        return NT_EPIPE;
    case ERROR_PIPE_NOT_CONNECTED: return NT_ENOTCONN;
    case ERROR_SEM_TIMEOUT:
    case WAIT_TIMEOUT:
        return NT_ETIMEDOUT;
    case ERROR_OPERATION_ABORTED: return NT_ECANCELED;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
    case ERROR_INVALID_FUNCTION:
        return NT_ENOSYS;
    case ERROR_BUFFER_OVERFLOW:
    case ERROR_INSUFFICIENT_BUFFER:
    case ERROR_MORE_DATA:
        return NT_ERANGE;
    case ERROR_HANDLE_EOF: return 0;
    default: return NT_EIO;
    }
}

int nt_errno_from_wsa(int e)
{
    switch (e) {
    case 0: return 0;
    case WSAEINTR: return NT_EINTR;
    case WSAEBADF:
    case WSAENOTSOCK: return NT_EBADF;
    case WSAEACCES: return NT_EACCES;
    case WSAEFAULT: return NT_EFAULT;
    case WSAEINVAL: return NT_EINVAL;
    case WSAEMFILE: return NT_EMFILE;
    case WSAEWOULDBLOCK: return NT_EAGAIN;
    case WSAEINPROGRESS: return NT_EINPROGRESS;
    case WSAEALREADY: return NT_EALREADY;
    case WSAEDESTADDRREQ: return NT_EDESTADDRREQ;
    case WSAEMSGSIZE: return NT_EMSGSIZE;
    case WSAEPROTONOSUPPORT: return NT_EPROTONOSUPPORT;
    case WSAEAFNOSUPPORT: return NT_EAFNOSUPPORT;
    case WSAEADDRINUSE: return NT_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return NT_EADDRNOTAVAIL;
    case WSAENETDOWN: return NT_ENETDOWN;
    case WSAENETUNREACH: return NT_ENETUNREACH;
    case WSAECONNABORTED: return NT_ECONNABORTED;
    case WSAECONNRESET: return NT_ECONNRESET;
    case WSAENOBUFS: return NT_ENOBUFS;
    case WSAEISCONN: return NT_EISCONN;
    case WSAENOTCONN: return NT_ENOTCONN;
    case WSAETIMEDOUT: return NT_ETIMEDOUT;
    case WSAECONNREFUSED: return NT_ECONNREFUSED;
    case WSAEHOSTUNREACH: return NT_EHOSTUNREACH;
    default: return NT_EIO;
    }
}

nt_sc_t nt_error(DWORD e)
{
    int value = nt_errno_from_win32(e);
    return value ? -value : 0;
}

nt_sc_t nt_last_error(void)
{
    return nt_error(GetLastError());
}

nt_sc_t nt_error_from_status(NTSTATUS status)
{
    typedef ULONG (WINAPI *rtl_status_fn)(NTSTATUS);
    static rtl_status_fn convert;
    if (!convert) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll)
            convert = (rtl_status_fn)(void *)GetProcAddress(ntdll,
                                                            "RtlNtStatusToDosError");
    }
    if (convert)
        return nt_error(convert(status));
    return -NT_EIO;
}
