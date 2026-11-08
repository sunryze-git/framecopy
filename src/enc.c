#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "clock.h"
#include "enc.h"
#include "util.h"

#define ENC_DEVICE "/dev/video-enc0"
#define CODED_BUFFERS 4
#define ALL_FREE ((1u << ENC_BUFFERS) - 1)

static struct {
    int fd;
    int frames[ENC_BUFFERS];
    uint32_t frame_size;
    unsigned free_frames;
    unsigned pending_packets;
    void *coded[CODED_BUFFERS];
} enc;

static void xioctl(unsigned long req, void *arg, const char *name)
{
    while (ioctl(enc.fd, req, arg) < 0)
        if (errno != EINTR)
            die("%s: %s", name, strerror(errno));
}

static void set_control(uint32_t id, int32_t value, const char *name)
{
    struct v4l2_control c = {.id = id, .value = value};

    xioctl(VIDIOC_S_CTRL, &c, name);
}

static void set_format(uint32_t type, uint32_t w, uint32_t h, uint32_t fourcc, struct v4l2_format *f)
{
    *f = (struct v4l2_format){.type = type};
    f->fmt.pix_mp.width = w;
    f->fmt.pix_mp.height = h;
    f->fmt.pix_mp.pixelformat = fourcc;
    f->fmt.pix_mp.num_planes = 1;
    xioctl(VIDIOC_S_FMT, f, "VIDIOC_S_FMT");
    if (f->fmt.pix_mp.pixelformat != fourcc)
        die("encoder does not support %.4s", (char *)&fourcc);
}

static int has_size(const struct v4l2_format *f, uint32_t w, uint32_t h)
{
    return f->fmt.pix_mp.width == w && f->fmt.pix_mp.height == h;
}

static void queue_coded(uint32_t index)
{
    struct v4l2_plane plane = {0};
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                            .memory = V4L2_MEMORY_MMAP,
                            .index = index,
                            .m.planes = &plane,
                            .length = 1};

    xioctl(VIDIOC_QBUF, &b, "VIDIOC_QBUF");
}

static void map_coded_buffers(void)
{
    struct v4l2_requestbuffers rb = {
        .count = CODED_BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP};

    xioctl(VIDIOC_REQBUFS, &rb, "VIDIOC_REQBUFS");
    if (rb.count < CODED_BUFFERS)
        die("encoder gave %u coded buffers", rb.count);
    for (uint32_t i = 0; i < CODED_BUFFERS; i++) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                .memory = V4L2_MEMORY_MMAP,
                                .index = i,
                                .m.planes = &plane,
                                .length = 1};

        xioctl(VIDIOC_QUERYBUF, &b, "VIDIOC_QUERYBUF");
        enc.coded[i] = mmap(NULL, plane.length, PROT_READ, MAP_SHARED, enc.fd, plane.m.mem_offset);
        if (enc.coded[i] == MAP_FAILED)
            die("mmap: %s", strerror(errno));
    }
}

struct enc_layout enc_init(uint32_t w, uint32_t h, unsigned fps, unsigned bitrate)
{
    struct v4l2_format coded, raw;

    enc.fd = open(ENC_DEVICE, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (enc.fd < 0)
        die("%s: %s", ENC_DEVICE, strerror(errno));

    set_format(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, w, h, V4L2_PIX_FMT_HEVC, &coded);
    set_format(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, w, h, V4L2_PIX_FMT_RGBA32, &raw);
    xioctl(VIDIOC_G_FMT, &coded, "VIDIOC_G_FMT");
    if (!has_size(&raw, w, h) || !has_size(&coded, w, h))
        die("encoder does not support %ux%u", w, h);

    struct v4l2_streamparm parm = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
    parm.parm.output.timeperframe = (struct v4l2_fract){1, fps};
    xioctl(VIDIOC_S_PARM, &parm, "VIDIOC_S_PARM");

    set_control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, "bitrate mode");
    set_control(V4L2_CID_MPEG_VIDEO_BITRATE, bitrate, "bitrate");
    set_control(V4L2_CID_MPEG_VIDEO_GOP_SIZE, fps, "GOP size");
    set_control(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, "B frames");
    set_control(V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, "header mode");

    map_coded_buffers();
    enc.frame_size = raw.fmt.pix_mp.plane_fmt[0].sizeimage;
    return (struct enc_layout){raw.fmt.pix_mp.plane_fmt[0].bytesperline, enc.frame_size};
}

void enc_start(const int fds[ENC_BUFFERS])
{
    struct v4l2_requestbuffers rb = {
        .count = ENC_BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_DMABUF};

    xioctl(VIDIOC_REQBUFS, &rb, "VIDIOC_REQBUFS");
    if (rb.count < ENC_BUFFERS)
        die("encoder gave %u frame buffers", rb.count);
    memcpy(enc.frames, fds, sizeof(enc.frames));
    enc.free_frames = ALL_FREE;

    for (uint32_t i = 0; i < CODED_BUFFERS; i++)
        queue_coded(i);
    uint32_t type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
}

void enc_submit(int buffer)
{
    struct v4l2_plane plane = {.bytesused = enc.frame_size, .length = enc.frame_size, .m.fd = enc.frames[buffer]};
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                            .memory = V4L2_MEMORY_DMABUF,
                            .index = buffer,
                            .m.planes = &plane,
                            .length = 1};
    /* Rate control spaces frames by timestamp and starves them when it is 0. */
    int64_t us = now_ns() / 1000;
    b.timestamp = (struct timeval){us / 1000000, us % 1000000};

    xioctl(VIDIOC_QBUF, &b, "VIDIOC_QBUF");
    enc.free_frames &= ~(1u << buffer);
    enc.pending_packets++;
}

static int dequeue(uint32_t type, uint32_t memory, struct v4l2_buffer *b, struct v4l2_plane *plane)
{
    *b = (struct v4l2_buffer){.type = type, .memory = memory, .m.planes = plane, .length = 1};
    if (ioctl(enc.fd, VIDIOC_DQBUF, b) == 0)
        return 1;
    if (errno != EAGAIN && errno != EINTR)
        die("VIDIOC_DQBUF: %s", strerror(errno));
    return 0;
}

static void write_all(const char *p, size_t len)
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

static void collect(int timeout_ms)
{
    struct pollfd p = {.fd = enc.fd, .events = POLLIN | POLLOUT};
    struct v4l2_buffer b;
    struct v4l2_plane plane;

    if (poll(&p, 1, timeout_ms) < 0 && errno != EINTR)
        die("poll: %s", strerror(errno));
    if (p.revents & POLLERR)
        die("encoder failed");

    while (dequeue(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, V4L2_MEMORY_DMABUF, &b, &plane))
        enc.free_frames |= 1u << b.index;
    while (dequeue(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_MEMORY_MMAP, &b, &plane)) {
        write_all((char *)enc.coded[b.index] + plane.data_offset, plane.bytesused - plane.data_offset);
        queue_coded(b.index);
        if (enc.pending_packets)
            enc.pending_packets--;
    }
}

int enc_acquire(void)
{
    while (!enc.free_frames)
        collect(-1);
    return __builtin_ctz(enc.free_frames);
}

void enc_drain_until(int64_t deadline_ns)
{
    for (int64_t left; (left = deadline_ns - now_ns()) > 0;) {
        if (enc.free_frames == ALL_FREE && !enc.pending_packets) {
            sleep_until(deadline_ns);
            return;
        }
        collect((left + 999999) / 1000000);
    }
}
