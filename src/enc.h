#ifndef ENC_H
#define ENC_H

#include <stdint.h>

#define ENC_BUFFERS 4

struct enc_layout {
    uint32_t stride;
    uint32_t size;
};

/* Opens the hardware HEVC encoder for RGBA frames of w x h. */
struct enc_layout enc_init(uint32_t w, uint32_t h, unsigned fps, unsigned bitrate);
/* Starts encoding from the given RGBA dmabufs, laid out as enc_init returned. */
void enc_start(const int fds[ENC_BUFFERS]);
/* Returns a frame buffer the encoder no longer reads, waiting for one if needed. */
int enc_acquire(void);
void enc_submit(int buffer);
/* Writes finished HEVC packets to stdout until the deadline. */
void enc_drain_until(int64_t deadline_ns);

#endif
