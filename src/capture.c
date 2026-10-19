#define _GNU_SOURCE
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "gpu.h"
#include "proto.h"
#include "util.h"

#define MAX_DIM 8192
#define MAX_FPS 240

static void usage(void)
{
    fputs("usage: framecopy-capture -s WxH -f fps [-e both|left|right]\n", stderr);
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
        if (dup2(sv[1], HELPER_SOCK_FD) < 0)
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

static void write_frame(const char *p, size_t len)
{
    while (len) {
        ssize_t n = write(STDOUT_FILENO, p, len);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            exit(errno == EPIPE ? 0 : 1);
        p += n;
        len -= n;
    }
}

static int64_t now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void sleep_until(int64_t ns)
{
    struct timespec t = {ns / 1000000000LL, ns % 1000000000LL};

    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
}

int main(int argc, char **argv)
{
    enum eye eye = EYE_BOTH;
    unsigned w = 0, h = 0, fps = 0;
    int len = 0;

    for (int c; (c = getopt(argc, argv, "e:s:f:")) != -1;) {
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
        else
            usage();
    }
    if (!w || !h || !fps || w > MAX_DIM || h > MAX_DIM || fps > MAX_FPS || w % 4 || h % 2)
        usage();

    signal(SIGPIPE, SIG_IGN);
    int sock = spawn_helper();
    gpu_init(w, h);

    int64_t next = now_ns();
    for (;;) {
        struct fb_info info;
        int fd = request_fb(sock, &info);

        if (info.status == FB_NO_PRIVILEGE)
            die("framecopy-kms needs cap_sys_admin");
        if (info.status == FB_UNSUPPORTED)
            die("multi-plane framebuffers are not supported");
        if (info.status == FB_OK) {
            write_frame(gpu_capture(&info, fd, eye), (size_t)w * h * 4);
            close(fd);
        }

        next += 1000000000LL / fps;
        if (next < now_ns())
            next = now_ns();
        else
            sleep_until(next);
    }
}
