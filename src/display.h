/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#ifndef __DRM_339_H__
#define __DRM_339_H__

#include <stdint.h>
#include <stdbool.h>
#include <aml_avsync.h>

enum frame_format {
  FRAME_FMT_NV12,
  FRAME_FMT_NV21,
  FRAME_FMT_AFBC,
};

typedef struct drm_frame drm_frame;

typedef int (*drm_frame_destroy)(drm_frame*);

struct rect {
  uint32_t x;
  uint32_t y;
  uint32_t w;
  uint32_t h;
};

//TODO: separete internal and external fields
struct drm_frame {
  void* gem;
  drm_frame_destroy destroy;
  uint32_t pts;
  bool last_flag;
  void* pri_sync;
  /* false is dropped by avsync */
  bool displayed;
  uint32_t duration;
  void* pri_dec;
  void* pri_drm;
  struct vframe sync_frame;
  struct rect window;
};

typedef int (*displayed_cb_func)(void* priv, void* handle);

void* display_engine_start(void * priv);
void display_engine_stop(void * handle);
int display_engine_register_cb(displayed_cb_func cb);

struct drm_frame* display_create_buffer(void *handle,
        unsigned int width, unsigned int height,
        enum frame_format format, int planes_count, bool secure);
int display_get_buffer_fds(struct drm_frame* drm_f, int *fd, int cnt);
int display_engine_show(void* handle, struct drm_frame* frame, struct rect* window);
int display_flush (void *handle);

int display_start_avsync(void *handle, enum sync_mode mode);
void display_stop_avsync(void *handle);

int display_set_pause(void *handle, bool pause);
#endif
