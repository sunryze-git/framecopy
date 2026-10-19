#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "proto.h"

union fd_control {
    struct cmsghdr align;
    char buf[CMSG_SPACE(sizeof(int))];
};

int fb_send(int sock, const struct fb_info *info, int fd)
{
    union fd_control ctl = {0};
    struct iovec iov = {(void *)info, sizeof(*info)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    ssize_t n;

    if (fd >= 0) {
        msg.msg_control = ctl.buf;
        msg.msg_controllen = sizeof(ctl.buf);

        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }

    do {
        n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof(*info) ? 0 : -1;
}

int fb_recv(int sock, struct fb_info *info, int *fd)
{
    union fd_control ctl;
    struct iovec iov = {info, sizeof(*info)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl.buf, .msg_controllen = sizeof(ctl.buf)};
    ssize_t n;

    do {
        n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(*info))
        return -1;

    *fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(fd, CMSG_DATA(c), sizeof(int));
    return 0;
}
