#define _GNU_SOURCE
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "clock.h"
#include "enc.h"
#include "gpu.h"
#include "proto.h"
#include "util.h"

/* Larger frames reset the whole Frame when the encoder starts. */
#define MAX_WIDTH 3840
#define MAX_HEIGHT 1920
#define MAX_FPS 240

static void usage(void)
{
    fputs("usage: framecopy-capture -s WxH -f fps -b bitrate [-e both|left|right]\n", stderr);
    exit(2);
}

static int spawn_helper(void)
{
    int sv[2];
    char self[PATH_MAX], path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);

    if (n < 0)
        die("readlink: %s", strerror(errno));
    self[n] = 0;
    if (snprintf(path, sizeof(path), "%s/framecopy-kms", dirname(self)) >= (int)sizeof(path))
        die("path too long");
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv))
        die("socketpair: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        if (sv[1] == HELPER_SOCK_FD ? fcntl(sv[1], F_SETFD, 0) : dup2(sv[1], HELPER_SOCK_FD) < 0)
            _exit(127);
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    return sv[0];
}

static int request_fb(int sock, struct fb_info *info)
{
    int fd;
    ssize_t n;

    do {
        n = send(sock, "x", 1, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n != 1 || fb_recv(sock, info, &fd))
        die("framecopy-kms exited");
    return fd;
}

static int parse_bitrate(const char *s, unsigned *bitrate)
{
    char *unit;
    unsigned long long v = strtoull(s, &unit, 10);

    if (unit == s)
        return -1;
    if (*unit == 'k' || *unit == 'K')
        v *= 1000;
    else if (*unit == 'm' || *unit == 'M')
        v *= 1000000;
    else if (*unit)
        return -1;
    if ((*unit && unit[1]) || v > UINT_MAX)
        return -1;
    *bitrate = v;
    return 0;
}

int main(int argc, char **argv)
{
    enum eye eye = EYE_BOTH;
    unsigned w = 0, h = 0, fps = 0, bitrate = 0;
    int len = 0;

    for (int c; (c = getopt(argc, argv, "e:s:f:b:")) != -1;) {
        if (c == 'e' && !strcmp(optarg, "both"))
            eye = EYE_BOTH;
        else if (c == 'e' && !strcmp(optarg, "left"))
            eye = EYE_LEFT;
        else if (c == 'e' && !strcmp(optarg, "right"))
            eye = EYE_RIGHT;
        else if (c == 's' && sscanf(optarg, "%ux%u%n", &w, &h, &len) == 2 && !optarg[len])
            continue;
        else if (c == 'f' && sscanf(optarg, "%u%n", &fps, &len) == 1 && !optarg[len])
            continue;
        else if (c == 'b' && !parse_bitrate(optarg, &bitrate))
            continue;
        else
            usage();
    }
    if (!w || !h || !fps || !bitrate || w > MAX_WIDTH || h > MAX_HEIGHT || fps > MAX_FPS || w % 4 || h % 2)
        usage();

    signal(SIGPIPE, SIG_IGN);
    int sock = spawn_helper();
    int frames[ENC_BUFFERS];
    gpu_init(w, h, enc_init(w, h, fps, bitrate), frames);
    enc_start(frames);

    int64_t next = now_ns();
    for (;;) {
        struct fb_info info;
        int fd = request_fb(sock, &info);

        if (info.status == FB_NO_PRIVILEGE)
            die("framecopy-kms needs cap_sys_admin");
        if (info.status == FB_UNSUPPORTED)
            die("multi-plane framebuffers are not supported");
        if (info.status == FB_OK) {
            int frame = enc_acquire();
            gpu_capture(&info, fd, eye, frame);
            enc_submit(frame);
            close(fd);
        }

        next += 1000000000LL / fps;
        if (next < now_ns())
            next = now_ns();
        enc_drain_until(next);
    }
}
