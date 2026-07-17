#include "nt/ntpriv.h"

#define NT_MSG_OOB 0x1
#define NT_MSG_PEEK 0x2
#define NT_MSG_DONTROUTE 0x4
#define NT_MSG_DONTWAIT 0x40
#define NT_MSG_WAITALL 0x100
#define NT_MSG_NOSIGNAL 0x4000
#define NT_MSG_MORE 0x8000
#define NT_MSG_WAITFORONE 0x10000
#define NT_SOL_SOCKET 1
#define NT_SO_TYPE 3
#define NT_SO_ERROR 4
#define NT_SO_DONTROUTE 5
#define NT_SO_BROADCAST 6
#define NT_SO_SNDBUF 7
#define NT_SO_RCVBUF 8
#define NT_SO_KEEPALIVE 9
#define NT_SO_REUSEADDR 2
#define NT_SO_LINGER 13
#define NT_SO_RCVTIMEO 20
#define NT_SO_SNDTIMEO 21
#define NT_SO_ACCEPTCONN 30
#define NT_IPV6_V6ONLY 26

static INIT_ONCE winsock_once = INIT_ONCE_STATIC_INIT;
static int winsock_ready;

/*
 * Do not link ws2_32's import library.  Its POSIX-compatible exports (bind,
 * connect, send, ...) collide with the public musl implementations when the
 * archive is linked whole.  Keeping every Winsock entry behind this private
 * table also guarantees that backend calls cannot accidentally recurse into
 * musl.
 */
static struct {
    HMODULE module;
    __typeof__(&WSAStartup) startup;
    __typeof__(&WSASocketW) socket_w;
    __typeof__(&WSADuplicateSocketW) duplicate_socket;
    __typeof__(&closesocket) close_socket;
    __typeof__(&ioctlsocket) ioctl_socket;
    __typeof__(&recv) receive;
    __typeof__(&send) transmit;
    __typeof__(&WSAPoll) poll;
    __typeof__(&bind) bind_socket;
    __typeof__(&connect) connect_socket;
    __typeof__(&listen) listen_socket;
    __typeof__(&accept) accept_socket;
    __typeof__(&sendto) send_to;
    __typeof__(&recvfrom) receive_from;
    __typeof__(&WSASendTo) send_message;
    __typeof__(&WSARecvFrom) receive_message;
    __typeof__(&getsockname) get_socket_name;
    __typeof__(&getpeername) get_peer_name;
    __typeof__(&shutdown) shutdown_socket;
    __typeof__(&setsockopt) set_socket_option;
    __typeof__(&getsockopt) get_socket_option;
    __typeof__(&WSAGetLastError) get_last_error;
} winsock;

#define NT_LOAD_WS(field, symbol) do { \
    winsock.field = (__typeof__(winsock.field))(void *) \
        GetProcAddress(winsock.module, symbol); \
    if (!winsock.field) return TRUE; \
} while (0)

#define WSAStartup winsock.startup
#define WSASocketW winsock.socket_w
#define WSADuplicateSocketW winsock.duplicate_socket
#define closesocket winsock.close_socket
#define ioctlsocket winsock.ioctl_socket
#define recv winsock.receive
#define send winsock.transmit
#define WSAPoll winsock.poll
#define bind winsock.bind_socket
#define connect winsock.connect_socket
#define listen winsock.listen_socket
#define accept winsock.accept_socket
#define sendto winsock.send_to
#define recvfrom winsock.receive_from
#define WSASendTo winsock.send_message
#define WSARecvFrom winsock.receive_message
#define getsockname winsock.get_socket_name
#define getpeername winsock.get_peer_name
#define shutdown winsock.shutdown_socket
#define setsockopt winsock.set_socket_option
#define getsockopt winsock.get_socket_option
#define WSAGetLastError winsock.get_last_error

static BOOL CALLBACK initialize_winsock(PINIT_ONCE once, PVOID parameter,
                                        PVOID *context)
{
    WSADATA data;
    (void)once;
    (void)parameter;
    (void)context;
    winsock.module = LoadLibraryW(L"ws2_32.dll");
    if (!winsock.module) return TRUE;
    NT_LOAD_WS(startup, "WSAStartup");
    NT_LOAD_WS(socket_w, "WSASocketW");
    NT_LOAD_WS(duplicate_socket, "WSADuplicateSocketW");
    NT_LOAD_WS(close_socket, "closesocket");
    NT_LOAD_WS(ioctl_socket, "ioctlsocket");
    NT_LOAD_WS(receive, "recv");
    NT_LOAD_WS(transmit, "send");
    NT_LOAD_WS(poll, "WSAPoll");
    NT_LOAD_WS(bind_socket, "bind");
    NT_LOAD_WS(connect_socket, "connect");
    NT_LOAD_WS(listen_socket, "listen");
    NT_LOAD_WS(accept_socket, "accept");
    NT_LOAD_WS(send_to, "sendto");
    NT_LOAD_WS(receive_from, "recvfrom");
    NT_LOAD_WS(send_message, "WSASendTo");
    NT_LOAD_WS(receive_message, "WSARecvFrom");
    NT_LOAD_WS(get_socket_name, "getsockname");
    NT_LOAD_WS(get_peer_name, "getpeername");
    NT_LOAD_WS(shutdown_socket, "shutdown");
    NT_LOAD_WS(set_socket_option, "setsockopt");
    NT_LOAD_WS(get_socket_option, "getsockopt");
    NT_LOAD_WS(get_last_error, "WSAGetLastError");
    winsock_ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    return TRUE;
}

static nt_sc_t ensure_winsock(void)
{
    InitOnceExecuteOnce(&winsock_once, initialize_winsock, 0, 0);
    return winsock_ready ? 0 : -NT_ENETDOWN;
}

nt_sc_t nt_wsa_error(void)
{
    int value;
    if (!winsock.get_last_error) return -NT_EIO;
    value = nt_errno_from_wsa(WSAGetLastError());
    return value ? -value : 0;
}

static int win_family(int family)
{
    if (family == NT_AF_INET6) return AF_INET6;
    if (family == NT_AF_INET) return AF_INET;
    if (family == NT_AF_UNIX) return AF_UNIX;
    return -1;
}

static int linux_family(int family)
{
    if (family == AF_INET6) return NT_AF_INET6;
    if (family == AF_INET) return NT_AF_INET;
    if (family == AF_UNIX) return NT_AF_UNIX;
    return family;
}

static nt_sc_t address_to_win(const void *address, nt_sc_t length,
                              SOCKADDR_STORAGE *storage, int *win_length)
{
    const unsigned short *family = address;
    int translated;
    if (!address || !win_length) return -NT_EFAULT;
    if (length < (nt_sc_t)sizeof *family ||
        length > (nt_sc_t)sizeof *storage)
        return -NT_EINVAL;
    translated = win_family(*family);
    if (translated < 0) return -NT_EAFNOSUPPORT;
    nt_memset(storage, 0, sizeof *storage);
    nt_memcpy(storage, address, (size_t)length);
    ((SOCKADDR *)storage)->sa_family = (ADDRESS_FAMILY)translated;
    *win_length = (int)length;
    return 0;
}

static nt_sc_t address_from_win(const SOCKADDR *address, int actual_length,
                                void *out, uint32_t *length)
{
    size_t copy;
    if (!length) return out ? -NT_EFAULT : 0;
    if (actual_length < 0) return -NT_EINVAL;
    copy = *length < (uint32_t)actual_length ? *length : (size_t)actual_length;
    if (out && copy) {
        nt_memcpy(out, address, copy);
        if (copy >= sizeof(unsigned short))
            *(unsigned short *)out =
                (unsigned short)linux_family(address->sa_family);
    }
    *length = (uint32_t)actual_length;
    return 0;
}

static struct nt_fd *socket_slot(nt_sc_t fd)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    return slot && slot->kind == NT_FD_SOCKET ? slot : 0;
}

nt_sc_t nt_socket_set_nonblocking(struct nt_fd *slot, int enabled)
{
    u_long value = enabled != 0;
    nt_sc_t r = ensure_winsock();
    if (r < 0) return r;
    if (!slot || slot->kind != NT_FD_SOCKET) return -NT_ENOTSOCK;
    if (ioctlsocket((SOCKET)(uintptr_t)slot->handle, FIONBIO, &value))
        return nt_wsa_error();
    return 0;
}

int nt_socket_close(HANDLE handle)
{
    if (ensure_winsock() < 0) return 0;
    return closesocket((SOCKET)(uintptr_t)handle) == 0;
}

nt_sc_t nt_socket_duplicate(HANDLE handle, int cloexec, HANDLE *copy)
{
    WSAPROTOCOL_INFOW protocol;
    SOCKET duplicate;
    nt_sc_t r = ensure_winsock();
    if (r < 0) return r;
    if (!copy) return -NT_EFAULT;
    if (WSADuplicateSocketW((SOCKET)(uintptr_t)handle,
                            GetCurrentProcessId(), &protocol) == SOCKET_ERROR)
        return nt_wsa_error();
    duplicate = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                           FROM_PROTOCOL_INFO, &protocol, 0,
                           cloexec ? WSA_FLAG_NO_HANDLE_INHERIT : 0);
    if (duplicate == INVALID_SOCKET) return nt_wsa_error();
    *copy = (HANDLE)(uintptr_t)duplicate;
    return 0;
}

nt_sc_t nt_socket_read(struct nt_fd *slot, void *buffer, uint64_t length)
{
    int result;
    nt_sc_t r = ensure_winsock();
    if (r < 0) return r;
    if (!slot || slot->kind != NT_FD_SOCKET) return -NT_ENOTSOCK;
    if (!buffer && length) return -NT_EFAULT;
    if (length > 0x7ffff000U) length = 0x7ffff000U;
    result = recv((SOCKET)(uintptr_t)slot->handle, buffer, (int)length, 0);
    return result == SOCKET_ERROR ? nt_wsa_error() : result;
}

nt_sc_t nt_socket_write(struct nt_fd *slot, const void *buffer,
                        uint64_t length)
{
    int result;
    nt_sc_t r = ensure_winsock();
    if (r < 0) return r;
    if (!slot || slot->kind != NT_FD_SOCKET) return -NT_ENOTSOCK;
    if (!buffer && length) return -NT_EFAULT;
    if (length > 0x7ffff000U) length = 0x7ffff000U;
    result = send((SOCKET)(uintptr_t)slot->handle, (const char *)buffer,
                  (int)length, 0);
    return result == SOCKET_ERROR ? nt_wsa_error() : result;
}

int nt_socket_poll(struct nt_fd *slot, short events, short *revents)
{
    WSAPOLLFD socket_poll;
    int result;
    if (!revents) return 0;
    *revents = 0;
    if (ensure_winsock() < 0 || !slot || slot->kind != NT_FD_SOCKET) {
        *revents = NT_POLLERR;
        return 1;
    }
    socket_poll.fd = (SOCKET)(uintptr_t)slot->handle;
    socket_poll.events = 0;
    socket_poll.revents = 0;
    if (events & NT_POLLIN) socket_poll.events |= POLLRDNORM;
    if (events & NT_POLLOUT) socket_poll.events |= POLLWRNORM;
    result = WSAPoll(&socket_poll, 1, 0);
    if (result == SOCKET_ERROR) {
        *revents = NT_POLLERR;
        return 1;
    }
    if (socket_poll.revents & (POLLRDNORM | POLLRDBAND))
        *revents |= NT_POLLIN;
    if (socket_poll.revents & (POLLWRNORM | POLLWRBAND))
        *revents |= NT_POLLOUT;
    if (socket_poll.revents & POLLERR) *revents |= NT_POLLERR;
    if (socket_poll.revents & POLLHUP) *revents |= NT_POLLHUP;
    if (socket_poll.revents & POLLNVAL) *revents |= NT_POLLNVAL;
    return *revents != 0;
}

static nt_sc_t socket_flags(int flags, int receive, int *out)
{
    const int accepted = NT_MSG_OOB | NT_MSG_PEEK | NT_MSG_DONTROUTE |
                         NT_MSG_DONTWAIT | NT_MSG_NOSIGNAL | NT_MSG_MORE |
                         (receive ? NT_MSG_WAITALL : 0);
    int translated = 0;
    if (!out) return -NT_EFAULT;
    if (flags & ~accepted) return -NT_ENOTSUP;
    if (flags & NT_MSG_OOB) translated |= MSG_OOB;
    if (flags & NT_MSG_PEEK) translated |= MSG_PEEK;
    if (flags & NT_MSG_DONTROUTE) translated |= MSG_DONTROUTE;
    if (receive && (flags & NT_MSG_WAITALL)) translated |= MSG_WAITALL;
    *out = translated;
    return 0;
}

static nt_sc_t temporary_nonblocking(struct nt_fd *slot, int flags)
{
    if (!(flags & NT_MSG_DONTWAIT) || (slot->flags & NT_O_NONBLOCK)) return 0;
    return nt_socket_set_nonblocking(slot, 1);
}

static void restore_blocking(struct nt_fd *slot, int flags)
{
    if ((flags & NT_MSG_DONTWAIT) && !(slot->flags & NT_O_NONBLOCK))
        nt_socket_set_nonblocking(slot, 0);
}

nt_sc_t nt_sys_socket(nt_sc_t domain_arg, nt_sc_t type_arg, nt_sc_t protocol)
{
    int domain = win_family((int)domain_arg);
    int type = (int)type_arg;
    int base_type = type & ~(NT_SOCK_NONBLOCK | NT_SOCK_CLOEXEC);
    SOCKET socket_handle;
    int fd;
    nt_sc_t r = ensure_winsock();
    if (r < 0) return r;
    if (domain < 0) return -NT_EAFNOSUPPORT;
    if (base_type != NT_SOCK_STREAM && base_type != NT_SOCK_DGRAM)
        return -NT_EPROTONOSUPPORT;
    if (type & ~(base_type | NT_SOCK_NONBLOCK | NT_SOCK_CLOEXEC))
        return -NT_EINVAL;
    socket_handle = WSASocketW(domain, base_type, (int)protocol, 0, 0,
                               WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket_handle == INVALID_SOCKET) return nt_wsa_error();
    fd = nt_fd_alloc((HANDLE)(uintptr_t)socket_handle, NT_FD_SOCKET,
                     NT_O_RDWR |
                     ((type & NT_SOCK_NONBLOCK) ? NT_O_NONBLOCK : 0),
                     (type & NT_SOCK_CLOEXEC) ? NT_FD_CLOEXEC : 0, 0);
    if (fd < 0) {
        closesocket(socket_handle);
        return fd;
    }
    if (type & NT_SOCK_NONBLOCK) {
        r = nt_socket_set_nonblocking(nt_fd_get(fd), 1);
        if (r < 0) {
            nt_fd_close(fd);
            return r;
        }
    }
    return fd;
}

static nt_sc_t finish_socketpair(int first, int second, int type, int *out)
{
    nt_sc_t r;
    if (type & NT_SOCK_NONBLOCK) {
        struct nt_fd *slot = nt_fd_get(first);
        r = nt_socket_set_nonblocking(slot, 1);
        if (r < 0) return r;
        slot->flags |= NT_O_NONBLOCK;
        slot = nt_fd_get(second);
        r = nt_socket_set_nonblocking(slot, 1);
        if (r < 0) return r;
        slot->flags |= NT_O_NONBLOCK;
    }
    out[0] = first;
    out[1] = second;
    return 0;
}

nt_sc_t nt_sys_socketpair(nt_sc_t domain, nt_sc_t type_arg,
                          nt_sc_t protocol, nt_sc_t sockets_arg)
{
    SOCKADDR_IN first_address, second_address;
    uint32_t address_length;
    int *out = (int *)(uintptr_t)sockets_arg;
    int type = (int)type_arg;
    int base_type = type & ~(NT_SOCK_NONBLOCK | NT_SOCK_CLOEXEC);
    int creation_type = base_type | (type & NT_SOCK_CLOEXEC);
    int listener = -1, first = -1, second = -1;
    nt_sc_t r = -NT_EIO;

    if (!out) return -NT_EFAULT;
    if (domain != NT_AF_UNIX) return -NT_EAFNOSUPPORT;
    if (base_type != NT_SOCK_STREAM && base_type != NT_SOCK_DGRAM)
        return -NT_EPROTONOSUPPORT;
    if (type & ~(base_type | NT_SOCK_NONBLOCK | NT_SOCK_CLOEXEC))
        return -NT_EINVAL;

    nt_memset(&first_address, 0, sizeof first_address);
    first_address.sin_family = NT_AF_INET;
    first_address.sin_addr.S_un.S_addr = 0x0100007fUL;
    address_length = sizeof first_address;

    if (base_type == NT_SOCK_STREAM) {
        listener = (int)nt_sys_socket(NT_AF_INET, creation_type, protocol);
        if (listener < 0) return listener;
        r = nt_sys_bind(listener, (nt_sc_t)(uintptr_t)&first_address,
                        sizeof first_address);
        if (r < 0) goto done;
        r = nt_sys_getsockname(listener, (nt_sc_t)(uintptr_t)&first_address,
                               (nt_sc_t)(uintptr_t)&address_length);
        if (r < 0) goto done;
        r = nt_sys_listen(listener, 1);
        if (r < 0) goto done;
        first = (int)nt_sys_socket(NT_AF_INET, creation_type, protocol);
        if (first < 0) {
            r = first;
            goto done;
        }
        r = nt_sys_connect(first, (nt_sc_t)(uintptr_t)&first_address,
                           sizeof first_address);
        if (r < 0) goto done;
        second = (int)nt_sys_accept4(listener, 0, 0,
                                    type & NT_SOCK_CLOEXEC);
        if (second < 0) {
            r = second;
            goto done;
        }
    } else {
        first = (int)nt_sys_socket(NT_AF_INET, creation_type, protocol);
        if (first < 0) return first;
        second = (int)nt_sys_socket(NT_AF_INET, creation_type, protocol);
        if (second < 0) {
            r = second;
            goto done;
        }
        r = nt_sys_bind(first, (nt_sc_t)(uintptr_t)&first_address,
                        sizeof first_address);
        if (r < 0) goto done;
        nt_memset(&second_address, 0, sizeof second_address);
        second_address.sin_family = NT_AF_INET;
        second_address.sin_addr.S_un.S_addr = 0x0100007fUL;
        r = nt_sys_bind(second, (nt_sc_t)(uintptr_t)&second_address,
                        sizeof second_address);
        if (r < 0) goto done;
        address_length = sizeof first_address;
        r = nt_sys_getsockname(first, (nt_sc_t)(uintptr_t)&first_address,
                               (nt_sc_t)(uintptr_t)&address_length);
        if (r < 0) goto done;
        address_length = sizeof second_address;
        r = nt_sys_getsockname(second, (nt_sc_t)(uintptr_t)&second_address,
                               (nt_sc_t)(uintptr_t)&address_length);
        if (r < 0) goto done;
        r = nt_sys_connect(first, (nt_sc_t)(uintptr_t)&second_address,
                           sizeof second_address);
        if (r < 0) goto done;
        r = nt_sys_connect(second, (nt_sc_t)(uintptr_t)&first_address,
                           sizeof first_address);
        if (r < 0) goto done;
    }

    r = finish_socketpair(first, second, type, out);
    if (r >= 0) {
        first = second = -1;
    }

done:
    if (listener >= 0) nt_fd_close(listener);
    if (first >= 0) nt_fd_close(first);
    if (second >= 0) nt_fd_close(second);
    return r;
}

nt_sc_t nt_sys_bind(nt_sc_t fd, nt_sc_t address, nt_sc_t length)
{
    struct nt_fd *slot = socket_slot(fd);
    SOCKADDR_STORAGE storage;
    int win_length;
    nt_sc_t r;
    if (!slot) return -NT_ENOTSOCK;
    r = address_to_win((const void *)(uintptr_t)address, length, &storage,
                       &win_length);
    if (r < 0) return r;
    if (bind((SOCKET)(uintptr_t)slot->handle, (SOCKADDR *)&storage,
             win_length) == SOCKET_ERROR)
        return nt_wsa_error();
    return 0;
}

nt_sc_t nt_sys_connect(nt_sc_t fd, nt_sc_t address, nt_sc_t length)
{
    struct nt_fd *slot = socket_slot(fd);
    SOCKADDR_STORAGE storage;
    int win_length;
    nt_sc_t r;
    if (!slot) return -NT_ENOTSOCK;
    r = address_to_win((const void *)(uintptr_t)address, length, &storage,
                       &win_length);
    if (r < 0) return r;
    if (connect((SOCKET)(uintptr_t)slot->handle, (SOCKADDR *)&storage,
                win_length) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) return -NT_EINPROGRESS;
        return -nt_errno_from_wsa(error);
    }
    return 0;
}

nt_sc_t nt_sys_listen(nt_sc_t fd, nt_sc_t backlog)
{
    struct nt_fd *slot = socket_slot(fd);
    if (!slot) return -NT_ENOTSOCK;
    if (listen((SOCKET)(uintptr_t)slot->handle, (int)backlog) == SOCKET_ERROR)
        return nt_wsa_error();
    return 0;
}

nt_sc_t nt_sys_accept4(nt_sc_t fd, nt_sc_t address_arg, nt_sc_t length_arg,
                       nt_sc_t flags)
{
    struct nt_fd *slot = socket_slot(fd);
    SOCKADDR_STORAGE address;
    uint32_t *length = (uint32_t *)(uintptr_t)length_arg;
    int win_length = sizeof address;
    SOCKET accepted;
    int new_fd;
    nt_sc_t r;
    if (!slot) return -NT_ENOTSOCK;
    if (flags & ~(NT_SOCK_NONBLOCK | NT_SOCK_CLOEXEC)) return -NT_EINVAL;
    if (address_arg && !length) return -NT_EFAULT;
    accepted = accept((SOCKET)(uintptr_t)slot->handle,
                      address_arg ? (SOCKADDR *)&address : 0,
                      address_arg ? &win_length : 0);
    if (accepted == INVALID_SOCKET) return nt_wsa_error();
    if (address_arg) {
        r = address_from_win((SOCKADDR *)&address, win_length,
                             (void *)(uintptr_t)address_arg, length);
        if (r < 0) {
            closesocket(accepted);
            return r;
        }
    }
    new_fd = nt_fd_alloc((HANDLE)(uintptr_t)accepted, NT_FD_SOCKET,
                         NT_O_RDWR |
                         ((flags & NT_SOCK_NONBLOCK) ? NT_O_NONBLOCK : 0),
                         (flags & NT_SOCK_CLOEXEC) ? NT_FD_CLOEXEC : 0, 0);
    if (new_fd < 0) {
        closesocket(accepted);
        return new_fd;
    }
    if (flags & NT_SOCK_NONBLOCK) {
        r = nt_socket_set_nonblocking(nt_fd_get(new_fd), 1);
        if (r < 0) {
            nt_fd_close(new_fd);
            return r;
        }
    }
    return new_fd;
}

nt_sc_t nt_sys_sendto(nt_sc_t fd, nt_sc_t buffer_arg, nt_sc_t length,
                      nt_sc_t flags, nt_sc_t address_arg,
                      nt_sc_t address_length)
{
    struct nt_fd *slot = socket_slot(fd);
    SOCKADDR_STORAGE address;
    SOCKADDR *address_ptr = 0;
    int win_length = 0, win_flags, result;
    nt_sc_t r;
    if (!slot) return -NT_ENOTSOCK;
    if ((!buffer_arg && length) || length < 0) return -NT_EFAULT;
    if (length > 0x7fffffff) length = 0x7fffffff;
    if (address_arg) {
        r = address_to_win((const void *)(uintptr_t)address_arg,
                           address_length, &address, &win_length);
        if (r < 0) return r;
        address_ptr = (SOCKADDR *)&address;
    }
    r = socket_flags((int)flags, 0, &win_flags);
    if (r < 0) return r;
    r = temporary_nonblocking(slot, (int)flags);
    if (r < 0) return r;
    result = sendto((SOCKET)(uintptr_t)slot->handle,
                    (const char *)(uintptr_t)buffer_arg, (int)length,
                    win_flags, address_ptr, win_length);
    r = result == SOCKET_ERROR ? nt_wsa_error() : result;
    restore_blocking(slot, (int)flags);
    return r;
}

nt_sc_t nt_sys_recvfrom(nt_sc_t fd, nt_sc_t buffer_arg, nt_sc_t length,
                        nt_sc_t flags, nt_sc_t address_arg,
                        nt_sc_t address_length_arg)
{
    struct nt_fd *slot = socket_slot(fd);
    SOCKADDR_STORAGE address;
    uint32_t *address_length =
        (uint32_t *)(uintptr_t)address_length_arg;
    int win_length = sizeof address, win_flags, result;
    nt_sc_t r;
    if (!slot) return -NT_ENOTSOCK;
    if ((!buffer_arg && length) || length < 0) return -NT_EFAULT;
    if (address_arg && !address_length) return -NT_EFAULT;
    if (length > 0x7fffffff) length = 0x7fffffff;
    r = socket_flags((int)flags, 1, &win_flags);
    if (r < 0) return r;
    r = temporary_nonblocking(slot, (int)flags);
    if (r < 0) return r;
    result = recvfrom((SOCKET)(uintptr_t)slot->handle,
                      (char *)(uintptr_t)buffer_arg, (int)length,
                      win_flags,
                      address_arg ? (SOCKADDR *)&address : 0,
                      address_arg ? &win_length : 0);
    if (result == SOCKET_ERROR) {
        r = nt_wsa_error();
    } else {
        r = result;
        if (address_arg)
            address_from_win((SOCKADDR *)&address, win_length,
                             (void *)(uintptr_t)address_arg, address_length);
    }
    restore_blocking(slot, (int)flags);
    return r;
}

static nt_sc_t message_buffers(const struct nt_msghdr *message,
                               WSABUF *buffers, DWORD *count)
{
    uint64_t i;
    if (!message || !buffers || !count) return -NT_EFAULT;
    if (message->msg_iovlen > 64) return -NT_EMSGSIZE;
    if (!message->msg_iov && message->msg_iovlen) return -NT_EFAULT;
    for (i = 0; i < message->msg_iovlen; ++i) {
        if (message->msg_iov[i].iov_len > 0xffffffffU)
            return -NT_EMSGSIZE;
        buffers[i].buf = message->msg_iov[i].iov_base;
        buffers[i].len = (ULONG)message->msg_iov[i].iov_len;
    }
    *count = (DWORD)message->msg_iovlen;
    return 0;
}

nt_sc_t nt_sys_sendmsg(nt_sc_t fd, nt_sc_t message_arg, nt_sc_t flags)
{
    struct nt_fd *slot = socket_slot(fd);
    const struct nt_msghdr *message =
        (const void *)(uintptr_t)message_arg;
    WSABUF buffers[64];
    SOCKADDR_STORAGE address;
    SOCKADDR *address_ptr = 0;
    DWORD count, sent = 0;
    int address_length = 0, win_flags;
    nt_sc_t r;
    if (!slot) return -NT_ENOTSOCK;
    if (!message) return -NT_EFAULT;
    if (message->msg_controllen) return -NT_ENOTSUP;
    r = message_buffers(message, buffers, &count);
    if (r < 0) return r;
    if (message->msg_name) {
        r = address_to_win(message->msg_name, message->msg_namelen,
                           &address, &address_length);
        if (r < 0) return r;
        address_ptr = (SOCKADDR *)&address;
    }
    r = socket_flags((int)flags, 0, &win_flags);
    if (r < 0) return r;
    r = temporary_nonblocking(slot, (int)flags);
    if (r < 0) return r;
    if (WSASendTo((SOCKET)(uintptr_t)slot->handle, buffers, count, &sent,
                  win_flags, address_ptr, address_length,
                  0, 0) == SOCKET_ERROR)
        r = nt_wsa_error();
    else
        r = sent;
    restore_blocking(slot, (int)flags);
    return r;
}

nt_sc_t nt_sys_recvmsg(nt_sc_t fd, nt_sc_t message_arg, nt_sc_t flags)
{
    struct nt_fd *slot = socket_slot(fd);
    struct nt_msghdr *message = (void *)(uintptr_t)message_arg;
    WSABUF buffers[64];
    SOCKADDR_STORAGE address;
    DWORD count, received = 0, win_flags;
    int address_length = sizeof address;
    nt_sc_t r;
    if (!slot) return -NT_ENOTSOCK;
    if (!message) return -NT_EFAULT;
    r = message_buffers(message, buffers, &count);
    if (r < 0) return r;
    {
        int translated;
        r = socket_flags((int)flags, 1, &translated);
        if (r < 0) return r;
        win_flags = (DWORD)translated;
    }
    r = temporary_nonblocking(slot, (int)flags);
    if (r < 0) return r;
    if (WSARecvFrom((SOCKET)(uintptr_t)slot->handle, buffers, count,
                    &received, &win_flags,
                    message->msg_name ? (SOCKADDR *)&address : 0,
                    message->msg_name ? &address_length : 0, 0, 0)
        == SOCKET_ERROR) {
        r = nt_wsa_error();
    } else {
        message->msg_flags = 0;
        message->msg_controllen = 0;
        if (message->msg_name)
            address_from_win((SOCKADDR *)&address, address_length,
                             message->msg_name, &message->msg_namelen);
        r = received;
    }
    restore_blocking(slot, (int)flags);
    return r;
}

static void update_mmsg_timeout(struct nt_timespec *timeout,
                                ULONGLONG deadline)
{
    ULONGLONG now, remaining;
    if (!timeout) return;
    now = GetTickCount64();
    remaining = now < deadline ? deadline - now : 0;
    timeout->tv_sec = (int64_t)(remaining / 1000);
    timeout->tv_nsec = (int64_t)(remaining % 1000) * 1000000;
}

nt_sc_t nt_sys_recvmmsg(nt_sc_t fd, nt_sc_t messages_arg, nt_sc_t count,
                        nt_sc_t flags, nt_sc_t timeout_arg)
{
    struct nt_mmsghdr *messages =
        (struct nt_mmsghdr *)(uintptr_t)messages_arg;
    struct nt_timespec *timeout =
        (struct nt_timespec *)(uintptr_t)timeout_arg;
    ULONGLONG deadline = 0;
    int wait_for_one = ((int)flags & NT_MSG_WAITFORONE) != 0;
    nt_sc_t i;

    if ((!messages && count) || count < 0 || count > 1024)
        return -NT_EINVAL;
    flags &= ~NT_MSG_WAITFORONE;
    if (timeout) {
        uint64_t milliseconds;
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= 1000000000)
            return -NT_EINVAL;
        milliseconds = (uint64_t)timeout->tv_sec * 1000 +
                       ((uint64_t)timeout->tv_nsec + 999999) / 1000000;
        if (milliseconds > 0x7fffffffU) milliseconds = 0x7fffffffU;
        deadline = GetTickCount64() + milliseconds;
    }

    for (i = 0; i < count; ++i) {
        nt_sc_t message_flags = flags;
        nt_sc_t r;
        if (i && wait_for_one) message_flags |= NT_MSG_DONTWAIT;
        if (timeout && !(message_flags & NT_MSG_DONTWAIT)) {
            struct nt_pollfd pollfd = {
                .fd = (int32_t)fd,
                .events = NT_POLLIN,
            };
            ULONGLONG now = GetTickCount64();
            nt_sc_t remaining = now < deadline
                ? (nt_sc_t)(deadline - now) : 0;
            r = nt_sys_poll((nt_sc_t)(uintptr_t)&pollfd, 1, remaining);
            if (r <= 0) {
                update_mmsg_timeout(timeout, deadline);
                return i ? i : r;
            }
        }
        r = nt_sys_recvmsg(fd,
                           (nt_sc_t)(uintptr_t)&messages[i].msg_hdr,
                           message_flags);
        if (r < 0) {
            update_mmsg_timeout(timeout, deadline);
            return i ? i : r;
        }
        messages[i].msg_len = (uint32_t)r;
    }
    update_mmsg_timeout(timeout, deadline);
    return i;
}

static nt_sc_t socket_name(nt_sc_t fd, nt_sc_t address_arg,
                           nt_sc_t length_arg, int peer)
{
    struct nt_fd *slot = socket_slot(fd);
    SOCKADDR_STORAGE address;
    uint32_t *length = (uint32_t *)(uintptr_t)length_arg;
    int win_length = sizeof address;
    int result;
    if (!slot) return -NT_ENOTSOCK;
    if (!address_arg || !length) return -NT_EFAULT;
    result = peer
        ? getpeername((SOCKET)(uintptr_t)slot->handle,
                      (SOCKADDR *)&address, &win_length)
        : getsockname((SOCKET)(uintptr_t)slot->handle,
                      (SOCKADDR *)&address, &win_length);
    if (result == SOCKET_ERROR) return nt_wsa_error();
    return address_from_win((SOCKADDR *)&address, win_length,
                            (void *)(uintptr_t)address_arg, length);
}

nt_sc_t nt_sys_getsockname(nt_sc_t fd, nt_sc_t address, nt_sc_t length)
{
    return socket_name(fd, address, length, 0);
}

nt_sc_t nt_sys_getpeername(nt_sc_t fd, nt_sc_t address, nt_sc_t length)
{
    return socket_name(fd, address, length, 1);
}

nt_sc_t nt_sys_shutdown(nt_sc_t fd, nt_sc_t how)
{
    struct nt_fd *slot = socket_slot(fd);
    if (!slot) return -NT_ENOTSOCK;
    if (how < 0 || how > 2) return -NT_EINVAL;
    if (shutdown((SOCKET)(uintptr_t)slot->handle, (int)how) == SOCKET_ERROR)
        return nt_wsa_error();
    return 0;
}

static int socket_option(int level, int option)
{
    if (level != NT_SOL_SOCKET) {
        if (level == 41 && option == NT_IPV6_V6ONLY) return IPV6_V6ONLY;
        return option;
    }
    switch (option) {
    case NT_SO_REUSEADDR: return SO_REUSEADDR;
    case NT_SO_TYPE: return SO_TYPE;
    case NT_SO_ERROR: return SO_ERROR;
    case NT_SO_DONTROUTE: return SO_DONTROUTE;
    case NT_SO_BROADCAST: return SO_BROADCAST;
    case NT_SO_SNDBUF: return SO_SNDBUF;
    case NT_SO_RCVBUF: return SO_RCVBUF;
    case NT_SO_KEEPALIVE: return SO_KEEPALIVE;
    case NT_SO_LINGER: return SO_LINGER;
    case NT_SO_RCVTIMEO: return SO_RCVTIMEO;
    case NT_SO_SNDTIMEO: return SO_SNDTIMEO;
    case NT_SO_ACCEPTCONN: return SO_ACCEPTCONN;
    default: return -1;
    }
}

static int socket_level(int level)
{
    return level == NT_SOL_SOCKET ? SOL_SOCKET : level;
}

nt_sc_t nt_sys_setsockopt(nt_sc_t fd, nt_sc_t level_arg, nt_sc_t option_arg,
                          nt_sc_t value_arg, nt_sc_t length)
{
    struct nt_fd *slot = socket_slot(fd);
    int level = socket_level((int)level_arg);
    int option = socket_option((int)level_arg, (int)option_arg);
    const char *value = (const char *)(uintptr_t)value_arg;
    int win_length = (int)length;
    DWORD milliseconds;
    LINGER win_linger;
    if (!slot) return -NT_ENOTSOCK;
    if (option < 0) return -NT_ENOTSUP;
    if ((!value && length) || length < 0 || length > 0x7fffffff)
        return -NT_EINVAL;
    if (level_arg == NT_SOL_SOCKET &&
        (option_arg == NT_SO_RCVTIMEO || option_arg == NT_SO_SNDTIMEO)) {
        const struct nt_timeval *timeout = (const void *)value;
        uint64_t total;
        if (!timeout || length < (nt_sc_t)sizeof *timeout) return -NT_EINVAL;
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0 ||
            timeout->tv_usec >= 1000000) return -NT_EINVAL;
        total = (uint64_t)timeout->tv_sec * 1000 +
                ((uint64_t)timeout->tv_usec + 999) / 1000;
        milliseconds = total > 0xffffffffU ? 0xffffffffU : (DWORD)total;
        value = (const char *)&milliseconds;
        win_length = sizeof milliseconds;
    } else if (level_arg == NT_SOL_SOCKET && option_arg == NT_SO_LINGER) {
        const int *linux_linger = (const int *)(const void *)value;
        if (!value || length < (nt_sc_t)(2 * sizeof(int)))
            return -NT_EINVAL;
        if (linux_linger[0] < 0 || linux_linger[1] < 0)
            return -NT_EINVAL;
        win_linger.l_onoff = linux_linger[0] != 0;
        win_linger.l_linger = linux_linger[1] > 0xffff
            ? 0xffff : (u_short)linux_linger[1];
        value = (const char *)&win_linger;
        win_length = sizeof win_linger;
    }
    if (setsockopt((SOCKET)(uintptr_t)slot->handle, level, option,
                   value, win_length) == SOCKET_ERROR)
        return nt_wsa_error();
    return 0;
}

nt_sc_t nt_sys_getsockopt(nt_sc_t fd, nt_sc_t level_arg, nt_sc_t option_arg,
                          nt_sc_t value_arg, nt_sc_t length_arg)
{
    struct nt_fd *slot = socket_slot(fd);
    int level = socket_level((int)level_arg);
    int option = socket_option((int)level_arg, (int)option_arg);
    char *value = (char *)(uintptr_t)value_arg;
    uint32_t *length = (uint32_t *)(uintptr_t)length_arg;
    int win_length;
    DWORD milliseconds;
    LINGER win_linger;
    if (!slot) return -NT_ENOTSOCK;
    if (option < 0) return -NT_ENOTSUP;
    if (!value || !length) return -NT_EFAULT;
    if (level_arg == NT_SOL_SOCKET &&
        (option_arg == NT_SO_RCVTIMEO || option_arg == NT_SO_SNDTIMEO)) {
        struct nt_timeval *timeout = (void *)value;
        win_length = sizeof milliseconds;
        if (*length < sizeof *timeout) return -NT_EINVAL;
        if (getsockopt((SOCKET)(uintptr_t)slot->handle, level, option,
                       (char *)&milliseconds, &win_length) == SOCKET_ERROR)
            return nt_wsa_error();
        timeout->tv_sec = milliseconds / 1000;
        timeout->tv_usec = (milliseconds % 1000) * 1000;
        *length = sizeof *timeout;
        return 0;
    }
    if (level_arg == NT_SOL_SOCKET && option_arg == NT_SO_LINGER) {
        int *linux_linger = (int *)(void *)value;
        if (*length < 2 * sizeof(int)) return -NT_EINVAL;
        win_length = sizeof win_linger;
        if (getsockopt((SOCKET)(uintptr_t)slot->handle, level, option,
                       (char *)&win_linger, &win_length) == SOCKET_ERROR)
            return nt_wsa_error();
        linux_linger[0] = win_linger.l_onoff != 0;
        linux_linger[1] = win_linger.l_linger;
        *length = 2 * sizeof(int);
        return 0;
    }
    if (level_arg == NT_SOL_SOCKET && option_arg == NT_SO_ERROR) {
        int win_error;
        if (*length < sizeof win_error) return -NT_EINVAL;
        win_length = sizeof win_error;
        if (getsockopt((SOCKET)(uintptr_t)slot->handle, level, option,
                       (char *)&win_error, &win_length) == SOCKET_ERROR)
            return nt_wsa_error();
        *(int *)(void *)value = nt_errno_from_wsa(win_error);
        *length = sizeof(int);
        return 0;
    }
    win_length = *length > 0x7fffffffU ? 0x7fffffff : (int)*length;
    if (getsockopt((SOCKET)(uintptr_t)slot->handle, level, option,
                   value, &win_length) == SOCKET_ERROR)
        return nt_wsa_error();
    *length = (uint32_t)win_length;
    return 0;
}
