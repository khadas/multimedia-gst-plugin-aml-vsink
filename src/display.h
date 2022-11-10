/* GStreamer
 * Copyright (C) 2020 Amlogic, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free SoftwareFoundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */
#ifndef __DISPLAY_339_H__
#define __DISPLAY_339_H__

#include <stdint.h>
#include <stdbool.h>
#include <aml_avsync.h>
#include <meson_drm_util.h>

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
  struct drm_buf *buf;
  drm_frame_destroy destroy;

  void *vaddr; /* for black frame use only */
  uint32_t pts;
  void* pri_sync;
  uint32_t duration;
  void* pri_dec;
  void* pri_drm;
  struct vframe sync_frame;
  struct rect source_window;
};

typedef int (*displayed_cb_func)(void* priv, void* handle, bool displayed, bool recycled);
typedef int (*pause_cb_func)(void* priv, uint32_t pts);
typedef int (*underflow_cb_func)(void* priv, uint32_t pts);

void *display_engine_start(void* priv, bool pip, bool low_latency);
void display_engine_stop(void * handle);
int display_engine_register_cb(displayed_cb_func cb);
int pause_pts_register_cb(pause_cb_func cb);
int display_underflow_register_cb(underflow_cb_func cb);

struct drm_frame* display_create_buffer(void *handle,
        unsigned int width, unsigned int height,
        enum frame_format format, int planes_count,
        bool secure, bool pip);
int display_get_buffer_fds(struct drm_frame *drm_f, int *fd, int cnt);
int display_engine_show(void *handle, struct drm_frame* frame, struct rect *src_window);
void display_engine_set_dst_rect(void *handle, struct rect *window);
int display_start_avsync(void *handle, enum sync_mode mode, int id, int delay);
void display_stop_avsync(void *handle);
int display_show_black_frame(void * handle);

int display_set_pause(void *handle, bool pause);
int display_set_pause_pts(void *handle, uint32_t pause_pts);
int display_set_speed(void *handle, float speed);
int display_set_checkunderflow(void *handle, bool underflow_check);
void display_engine_refresh(void* handle, struct rect *dst, struct rect *src);
#endif
