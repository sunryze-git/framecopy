#ifndef GPU_H
#define GPU_H

#include <stdint.h>

#include "enc.h"
#include "proto.h"

enum eye { EYE_BOTH, EYE_LEFT, EYE_RIGHT };

/* Creates the NV12 frame buffers in the encoder's layout and exports them as dmabufs. */
void gpu_init(uint32_t out_w, uint32_t out_h, struct enc_layout layout, int fds[ENC_BUFFERS]);
/* Scales the framebuffer into frame buffer `buffer` and waits until it is written. */
void gpu_capture(const struct fb_info *info, int fd, enum eye eye, int buffer);

#endif
