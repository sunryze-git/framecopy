#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "proto.h"
#include "util.h"

static void release_fb(int drm, drmModeFB2 *fb)
{
    for (int i = 0; i < 4; i++) {
        int seen = 0;
        for (int j = 0; j < i; j++)
            seen |= fb->handles[j] == fb->handles[i];
        if (fb->handles[i] && !seen)
            drmCloseBufferHandle(drm, fb->handles[i]);
    }
    drmModeFreeFB2(fb);
}

static uint64_t area(const drmModeFB2 *fb)
{
    return (uint64_t)fb->width * fb->height;
}

static drmModeFB2 *largest_fb(int drm, const drmModePlaneRes *res)
{
    drmModeFB2 *best = NULL;

    for (uint32_t i = 0; i < res->count_planes; i++) {
        drmModePlane *pl = drmModeGetPlane(drm, res->planes[i]);
        drmModeFB2 *fb = pl && pl->fb_id ? drmModeGetFB2(drm, pl->fb_id) : NULL;

        drmModeFreePlane(pl);
        if (!fb)
            continue;
        if (best && area(best) >= area(fb)) {
            release_fb(drm, fb);
            continue;
        }
        if (best)
            release_fb(drm, best);
        best = fb;
    }
    return best;
}

static int serve(int drm, const drmModePlaneRes *res)
{
    struct fb_info info = {.status = FB_NONE};
    int prime = -1;
    drmModeFB2 *fb = largest_fb(drm, res);

    if (fb) {
        info.fb_id = fb->fb_id;
        info.width = fb->width;
        info.height = fb->height;
        info.fourcc = fb->pixel_format;
        info.pitch = fb->pitches[0];
        info.offset = fb->offsets[0];
        info.modifier = fb->modifier;

        if (!fb->handles[0])
            info.status = FB_NO_PRIVILEGE;
        else if (fb->handles[1])
            info.status = FB_UNSUPPORTED;
        else if (!drmPrimeHandleToFD(drm, fb->handles[0], O_RDONLY | O_CLOEXEC, &prime)) {
            info.status = FB_OK;
            info.size = lseek(prime, 0, SEEK_END);
        }
        release_fb(drm, fb);
    }

    int rc = fb_send(HELPER_SOCK_FD, &info, prime);
    if (prime >= 0)
        close(prime);
    return rc;
}

int main(void)
{
    int drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm < 0)
        die("/dev/dri/card0: %s", strerror(errno));
    drmSetClientCap(drm, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    drmModePlaneRes *res = drmModeGetPlaneResources(drm);
    if (!res)
        die("no display planes");

    char req;
    for (;;) {
        ssize_t n = recv(HELPER_SOCK_FD, &req, 1, 0);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || serve(drm, res))
            return 0;
    }
}
