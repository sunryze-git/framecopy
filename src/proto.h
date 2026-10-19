#ifndef PROTO_H
#define PROTO_H

#include <stdint.h>

#define HELPER_SOCK_FD 3

enum fb_status {
    FB_OK,
    FB_NONE,
    FB_NO_PRIVILEGE,
    FB_UNSUPPORTED,
};

struct fb_info {
    uint32_t status;
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t pitch;
    uint32_t offset;
    uint32_t reserved;
    uint64_t modifier;
    uint64_t size;
};

/* The dmabuf fd travels as SCM_RIGHTS, pass -1 to send none. */
int fb_send(int sock, const struct fb_info *info, int fd);
/* Sets *fd to the received dmabuf, or -1 if the reply carried none. */
int fb_recv(int sock, struct fb_info *info, int *fd);

#endif
