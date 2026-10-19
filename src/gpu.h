#ifndef GPU_H
#define GPU_H

#include <stdint.h>

#include "proto.h"

enum eye { EYE_BOTH, EYE_LEFT, EYE_RIGHT };

void gpu_init(uint32_t out_w, uint32_t out_h);
/* Returns the scaled BGRA frame, valid until the next call. */
const void *gpu_capture(const struct fb_info *info, int fd, enum eye eye);

#endif
