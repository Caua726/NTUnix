#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int udp_loopback(void)
{
    static const char payload[] = "musl-nt udp";
    struct sockaddr_in receiver_address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    struct sockaddr_in source_address;
    socklen_t receiver_length = sizeof receiver_address;
    socklen_t source_length = sizeof source_address;
    struct pollfd pollfd;
    char buffer[32] = {0};
    int receiver = -1, sender = -1;
    int socket_type = 0;
    socklen_t option_length = sizeof socket_type;
    int ok = 0;

    receiver = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (receiver < 0) goto done;
    if (bind(receiver, (struct sockaddr *)&receiver_address,
             sizeof receiver_address))
        goto done;
    if (getsockname(receiver, (struct sockaddr *)&receiver_address,
                    &receiver_length))
        goto done;
    if (!receiver_address.sin_port) goto done;

    sender = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sender < 0) goto done;
    if (getsockopt(sender, SOL_SOCKET, SO_TYPE, &socket_type, &option_length))
        goto done;
    if (socket_type != SOCK_DGRAM) goto done;
    if (sendto(sender, payload, sizeof payload - 1, 0,
               (struct sockaddr *)&receiver_address,
               sizeof receiver_address) != (ssize_t)(sizeof payload - 1))
        goto done;

    pollfd.fd = receiver;
    pollfd.events = POLLIN;
    pollfd.revents = 0;
    if (poll(&pollfd, 1, 1000) != 1 || !(pollfd.revents & POLLIN))
        goto done;
    if (recvfrom(receiver, buffer, sizeof buffer, 0,
                 (struct sockaddr *)&source_address,
                 &source_length) != (ssize_t)(sizeof payload - 1))
        goto done;
    if (source_address.sin_family != AF_INET ||
        memcmp(buffer, payload, sizeof payload - 1))
        goto done;
    ok = 1;

done:
    if (sender >= 0) close(sender);
    if (receiver >= 0) close(receiver);
    return ok;
}

static int tcp_loopback(void)
{
    static const char request[] = "tcp request";
    static const char waitall[] = "waitall";
    static const char response_a[] = "musl-nt ";
    static const char response_b[] = "tcp";
    struct sockaddr_in server_address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    struct sockaddr_in peer_address;
    socklen_t server_length = sizeof server_address;
    socklen_t peer_length = sizeof peer_address;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    struct timeval actual_timeout = {0};
    socklen_t timeout_length = sizeof actual_timeout;
    struct pollfd pollfd;
    struct iovec send_iov[2] = {
        {.iov_base = (void *)response_a, .iov_len = sizeof response_a - 1},
        {.iov_base = (void *)response_b, .iov_len = sizeof response_b - 1},
    };
    struct iovec receive_iov[2];
    struct msghdr send_message = {
        .msg_iov = send_iov,
        .msg_iovlen = 2,
    };
    struct msghdr receive_message = {0};
    char request_buffer[32] = {0};
    char waitall_buffer[sizeof waitall] = {0};
    char response_buffer_a[9] = {0};
    char response_buffer_b[4] = {0};
    int server = -1, client = -1, accepted = -1, duplicate = -1;
    int enabled = 1;
    int ok = 0;

    server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) goto done;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR,
                   &enabled, sizeof enabled))
        goto done;
    if (bind(server, (struct sockaddr *)&server_address,
             sizeof server_address))
        goto done;
    if (getsockname(server, (struct sockaddr *)&server_address,
                    &server_length))
        goto done;
    if (!server_address.sin_port || listen(server, 1)) goto done;

    client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client < 0) goto done;
    if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof timeout))
        goto done;
    if (getsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   &actual_timeout, &timeout_length))
        goto done;
    if (actual_timeout.tv_sec != 0 ||
        actual_timeout.tv_usec < timeout.tv_usec)
        goto done;
    if (connect(client, (struct sockaddr *)&server_address,
                sizeof server_address))
        goto done;

    accepted = accept4(server, (struct sockaddr *)&peer_address,
                       &peer_length, SOCK_CLOEXEC);
    if (accepted < 0 || peer_address.sin_family != AF_INET) goto done;
    peer_length = sizeof peer_address;
    if (getpeername(accepted, (struct sockaddr *)&peer_address, &peer_length))
        goto done;

    if (write(client, request, sizeof request - 1) !=
        (ssize_t)(sizeof request - 1))
        goto done;
    pollfd.fd = accepted;
    pollfd.events = POLLIN;
    pollfd.revents = 0;
    if (poll(&pollfd, 1, 1000) != 1 || !(pollfd.revents & POLLIN))
        goto done;
    if (recv(accepted, request_buffer, 1, MSG_PEEK) != 1 ||
        request_buffer[0] != request[0])
        goto done;
    request_buffer[0] = 0;
    if (read(accepted, request_buffer, sizeof request_buffer) !=
        (ssize_t)(sizeof request - 1))
        goto done;
    if (memcmp(request_buffer, request, sizeof request - 1)) goto done;

    if (send(client, waitall, 3, 0) != 3 ||
        send(client, waitall + 3, sizeof waitall - 1 - 3, 0) !=
            (ssize_t)(sizeof waitall - 1 - 3))
        goto done;
    if (recv(accepted, waitall_buffer, sizeof waitall - 1, MSG_WAITALL) !=
        (ssize_t)(sizeof waitall - 1))
        goto done;
    if (memcmp(waitall_buffer, waitall, sizeof waitall - 1)) goto done;

    duplicate = dup(accepted);
    if (duplicate < 0) goto done;
    close(accepted);
    accepted = duplicate;
    duplicate = -1;

    if (sendmsg(accepted, &send_message, 0) !=
        (ssize_t)(sizeof response_a + sizeof response_b - 2))
        goto done;
    receive_iov[0].iov_base = response_buffer_a;
    receive_iov[0].iov_len = sizeof response_a - 1;
    receive_iov[1].iov_base = response_buffer_b;
    receive_iov[1].iov_len = sizeof response_b - 1;
    receive_message.msg_iov = receive_iov;
    receive_message.msg_iovlen = 2;
    if (recvmsg(client, &receive_message, 0) !=
        (ssize_t)(sizeof response_a + sizeof response_b - 2))
        goto done;
    if (memcmp(response_buffer_a, response_a, sizeof response_a - 1) ||
        memcmp(response_buffer_b, response_b, sizeof response_b - 1))
        goto done;
    if (shutdown(accepted, SHUT_WR)) goto done;
    ok = 1;

done:
    if (duplicate >= 0) close(duplicate);
    if (accepted >= 0) close(accepted);
    if (client >= 0) close(client);
    if (server >= 0) close(server);
    return ok;
}

static int host_resolution(void)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *addresses = 0;
    struct sockaddr_in *address;
    char host[64] = {0};
    int ok = 0;

    if (getaddrinfo("localhost", 0, &hints, &addresses)) goto done;
    if (!addresses || addresses->ai_family != AF_INET ||
        addresses->ai_addrlen < sizeof *address)
        goto done;
    address = (struct sockaddr_in *)addresses->ai_addr;
    if (address->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) goto done;
    if (getnameinfo(addresses->ai_addr, addresses->ai_addrlen,
                    host, sizeof host, 0, 0, NI_NAMEREQD))
        goto done;
    if (strcmp(host, "localhost")) goto done;
    ok = 1;

done:
    freeaddrinfo(addresses);
    return ok;
}

static int socketpair_loopback(void)
{
    static const char stream_payload[] = "stream-pair";
    static const char dgram_payload_a[] = "dgram-a";
    static const char dgram_payload_b[] = "dgram-b";
    struct iovec send_iov[2] = {
        {.iov_base = (void *)dgram_payload_a,
         .iov_len = sizeof dgram_payload_a - 1},
        {.iov_base = (void *)dgram_payload_b,
         .iov_len = sizeof dgram_payload_b - 1},
    };
    struct iovec receive_iov[2];
    struct mmsghdr sent[2] = {
        {.msg_hdr = {.msg_iov = &send_iov[0], .msg_iovlen = 1}},
        {.msg_hdr = {.msg_iov = &send_iov[1], .msg_iovlen = 1}},
    };
    struct mmsghdr received[2] = {0};
    struct timespec timeout = {.tv_sec = 1};
    char buffer[32] = {0};
    char dgram_buffer_a[16] = {0};
    char dgram_buffer_b[16] = {0};
    int stream[2] = {-1, -1};
    int dgram[2] = {-1, -1};
    int ok = 0;

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, stream))
        goto done;
    if (write(stream[0], stream_payload, sizeof stream_payload - 1) !=
        (ssize_t)(sizeof stream_payload - 1))
        goto done;
    if (read(stream[1], buffer, sizeof buffer) !=
        (ssize_t)(sizeof stream_payload - 1))
        goto done;
    if (memcmp(buffer, stream_payload, sizeof stream_payload - 1)) goto done;

    memset(buffer, 0, sizeof buffer);
    if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, dgram))
        goto done;
    if (sendmmsg(dgram[1], sent, 2, 0) != 2)
        goto done;
    receive_iov[0].iov_base = dgram_buffer_a;
    receive_iov[0].iov_len = sizeof dgram_buffer_a;
    receive_iov[1].iov_base = dgram_buffer_b;
    receive_iov[1].iov_len = sizeof dgram_buffer_b;
    received[0].msg_hdr.msg_iov = &receive_iov[0];
    received[0].msg_hdr.msg_iovlen = 1;
    received[1].msg_hdr.msg_iov = &receive_iov[1];
    received[1].msg_hdr.msg_iovlen = 1;
    if (recvmmsg(dgram[0], received, 2, MSG_WAITFORONE, &timeout) != 2)
        goto done;
    if (received[0].msg_len != sizeof dgram_payload_a - 1 ||
        received[1].msg_len != sizeof dgram_payload_b - 1)
        goto done;
    if (memcmp(dgram_buffer_a, dgram_payload_a, sizeof dgram_payload_a - 1) ||
        memcmp(dgram_buffer_b, dgram_payload_b, sizeof dgram_payload_b - 1))
        goto done;
    ok = 1;

done:
    if (dgram[0] >= 0) close(dgram[0]);
    if (dgram[1] >= 0) close(dgram[1]);
    if (stream[0] >= 0) close(stream[0]);
    if (stream[1] >= 0) close(stream[1]);
    return ok;
}

int main(void)
{
    int udp = udp_loopback();
    int tcp = tcp_loopback();
    int resolver = host_resolution();
    int pair = socketpair_loopback();
    if (udp && tcp && resolver && pair) errno = 0;
    printf("udp-loopback=%s tcp-loopback=%s socketpair=%s hosts=%s errno=%d\n",
           udp ? "ok" : "FAIL", tcp ? "ok" : "FAIL",
           pair ? "ok" : "FAIL",
           resolver ? "ok" : "FAIL", errno);
    return !(udp && tcp && resolver && pair);
}
