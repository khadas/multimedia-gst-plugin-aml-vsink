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
#ifndef _V4L_DEC_H_
#define _V4L_DEC_H_

#include <linux/videodev2.h>
#include <stdint.h>
#include <stdbool.h>
#include <gst/gstbuffer.h>
#include "display.h"

enum vdec_dw_mode {
  VDEC_DW_AFBC_ONLY = 0,
  VDEC_DW_AFBC_1_1_DW = 1,
  VDEC_DW_AFBC_1_4_DW = 2,
  VDEC_DW_AFBC_x2_1_4_DW = 3,
  VDEC_DW_AFBC_1_2_DW = 4,
  VDEC_DW_NO_AFBC = 16,
  VDEC_DW_AFBC_AUTO_1_2 = 0x100,
  VDEC_DW_AFBC_AUTO_1_4 = 0x200
};

struct output_buffer {
  struct v4l2_buffer buf;
  struct v4l2_plane plane;
  uint8_t *vaddr;
  uint32_t size;
  bool queued;
  uint32_t id;

  uint32_t used;
  GstBuffer *gstbuf;
};

struct capture_buffer {
  struct v4l2_buffer buf;
  struct v4l2_plane plane[2];
  uint8_t *vaddr[2];
  int gem_fd[2];
  bool displayed;
  uint32_t id;

  bool free_on_recycle;
  void *drm_handle;
  struct drm_frame *drm_frame;
};

struct hdr_meta {
  bool haveColorimetry;
  int Colorimetry[4];
  bool haveMasteringDisplay;
  float MasteringDisplay[10];
  bool haveContentLightLevel;
  int ContentLightLevel[2];
};

int v4l_dec_open(bool sanity_check);
int v4l_reg_event(int fd);
int v4l_unreg_event(int fd);

struct v4l2_fmtdesc* v4l_get_output_port_formats(int fd, uint32_t *num);
struct v4l2_fmtdesc* v4l_get_capture_port_formats(int fd, uint32_t *num);

struct output_buffer** v4l_setup_output_port (int fd, uint32_t mode,
    uint32_t *buf_cnt);
struct capture_buffer** v4l_setup_capture_port (int fd, uint32_t *buf_cnt,
    uint32_t dw_mode, void *drm_handle,
    uint32_t *coded_w, uint32_t *coded_h,
    bool secure, bool pip, bool is_2k_only);

void recycle_output_port_buffer (int fd, struct output_buffer **ob, uint32_t num);
int recycle_capture_port_buffer (int fd, struct capture_buffer **cb, uint32_t num);

int v4l_dec_dw_config(int fd, uint32_t fmt, uint32_t dw_mode, bool low_latency, bool only_2k, float frame_rate);
int v4l_dec_config(int fd, bool secure, uint32_t fmt, uint32_t dw_mode,
    struct hdr_meta *hdr, bool is_2k_only, float frame_rate, bool disable_dw_scale);
int v4l_set_output_format(int fd, uint32_t format, int w, int h, bool only_2k);
int v4l_set_secure_mode(int fd, int w, int h, bool secure);

int v4l_queue_capture_buffer(int fd, struct capture_buffer *cb);
#endif
