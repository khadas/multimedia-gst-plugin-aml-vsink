/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/prctl.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libsync.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>
#include <meson_drm.h>
#include <gst/gstinfo.h>

#include "aml_avsync.h"
#include "aml_avsync_log.h"
#include "display.h"

GST_DEBUG_CATEGORY_EXTERN(gst_aml_vsink_debug);
#define GST_CAT_DEFAULT gst_aml_vsink_debug

struct video_disp {
  struct drm_display *drm;
  bool started;
  pthread_t disp_t;
  bool last_frame;
  int drm_mode_set;
  void *priv;
  pthread_mutex_t avsync_lock;
  uint32_t pause_pts;
  struct drm_frame *black_frame;

  /* trick play */
  bool speed_pending;
  float speed;

  /* avsync */
  void * avsync;
  int session;
};

static struct drm_frame* create_black_frame (void* handle,
    unsigned int width, unsigned int height, bool pip);
static void destroy_black_frame (struct drm_frame *frame);
static int frame_destroy(struct drm_frame* drm_f);

displayed_cb_func display_cb;
pause_cb_func pause_cb;

static void display_res_change_cb(void *p)
{
  //struct video_disp *disp = p;
  //TODO
  return;
}

void *display_engine_start(void* priv, bool pip)
{
  struct video_disp *disp;
  struct drm_display *drm;

  disp = (struct video_disp *)calloc (1, sizeof(*disp));
  if (!disp) {
    GST_ERROR ("oom");
  }
  drm = drm_display_init();
  if (!drm) {
    GST_ERROR ("drm_display_init fail");
    goto error;
  }

  drm_display_register_done_cb (drm, display_res_change_cb, disp);
  disp->drm = drm;
  disp->priv = priv;
  disp->pause_pts = -1;
  disp->session = -1;
  pthread_mutex_init (&disp->avsync_lock, NULL);

  disp->black_frame = create_black_frame (disp, 64, 64, pip);
  /* avsync log level */
  log_set_level(LOG_INFO);
  return disp;
error:
  free (disp);
  return NULL;
}

static void pause_pts_cb(uint32_t pts, void* priv)
{
  struct video_disp * disp = priv;

  if (pause_cb)
    pause_cb (disp->priv, pts);

  /* only trigger once */
  disp->pause_pts = -1;
}

int display_start_avsync(void *handle, enum sync_mode mode, int id)
{
  struct video_disp * disp = handle;
  int ret = 0;
  struct video_config config;

  pthread_mutex_lock (&disp->avsync_lock);
  if (mode == AV_SYNC_MODE_VMASTER) {
    int session_id;

    disp->session = av_sync_open_session(&session_id);
    if (disp->session < 0) {
      GST_ERROR ("create avsync session fail\n");
      ret = -1;
      goto exit;
    }
    id = session_id;
    GST_WARNING ("session ID %d", id);
  }

  disp->avsync = av_sync_create(id, mode, AV_SYNC_TYPE_VIDEO, 2);
  if (!disp->avsync) {
    GST_ERROR ("create avsync fails\n");
    ret = -1;
    goto exit;
  }

  config.delay = 2;
  av_sync_video_config(disp->avsync, &config);

  if (disp->speed_pending) {
    disp->speed_pending = false;
    av_sync_set_speed (disp->avsync, disp->speed);
  }
  pthread_mutex_unlock (&disp->avsync_lock);

  if (disp->pause_pts != -1) {
    av_sync_set_pause_pts_cb (disp->avsync, pause_pts_cb, disp);
    av_sync_set_pause_pts (disp->avsync, disp->pause_pts);
  }
  return 0;

exit:
  pthread_mutex_unlock (&disp->avsync_lock);
  return ret;
}

void display_stop_avsync(void *handle)
{
  struct video_disp * disp = handle;

  GST_WARNING ("stop avsync");
  pthread_mutex_lock (&disp->avsync_lock);
  if (disp->avsync) {
    av_sync_destroy (disp->avsync);
    disp->avsync = NULL;
  }
  if (disp->session > 0) {
    av_sync_close_session(disp->session);
    disp->session = -1;
  }
  disp->speed_pending = false;
  pthread_mutex_unlock (&disp->avsync_lock);
}

static int frame_destroy(struct drm_frame* drm_f)
{
  int rc;
  struct drm_buf *gem_buf = drm_f->buf;

  rc = drm_free_buf(gem_buf);
  free(drm_f);
  return rc;
}

static struct drm_frame* create_black_frame (void* handle,
    unsigned int width, unsigned int height, bool pip)
{
  struct video_disp *disp = handle;
  struct drm_buf *gem_buf;
  struct drm_buf_metadata info;
  struct drm_frame* frame = calloc(1, sizeof(*frame));

  if (!frame) {
    GST_ERROR ("oom\n");
    return NULL;
  }

  memset(&info, 0 , sizeof(info));

  /* use single planar for black frame */
  info.width = width;
  info.height = height;
  info.flags = 0;
  info.fourcc = DRM_FORMAT_YUYV;

  if (!pip)
    info.flags |= MESON_USE_VD1;
  else
    info.flags |= MESON_USE_VD2;

  gem_buf = drm_alloc_buf(disp->drm, &info);
  if (!gem_buf) {
    GST_ERROR ("Unable to alloc drm buf\n");
    goto error;
  }

  frame->buf = gem_buf;
  frame->pri_drm = handle;
  frame->destroy = frame_destroy;

  frame->vaddr = mmap (NULL, width * height * 2, PROT_WRITE,
      MAP_SHARED, gem_buf->fd[0], gem_buf->offsets[0]);

  if (frame->vaddr == MAP_FAILED) {
    GST_ERROR ("mmap fail %d", errno);
    drm_free_buf (gem_buf);
    goto error;
  }

  /* full screen black frame */
  memset (frame->vaddr, 0, width * height * 2);
  gem_buf->crtc_x = 0;
  gem_buf->crtc_y = 0;
  gem_buf->crtc_w = -1;
  gem_buf->crtc_h = -1;

  return frame;
error:
  if (frame) free (frame);
  return NULL;
}

static void destroy_black_frame (struct drm_frame *frame)
{
  if (!frame)
    return;

  munmap (frame->vaddr, frame->buf->width * frame->buf->height * 2);
  frame_destroy (frame);
}

struct drm_frame* display_create_buffer(void* handle,
    unsigned int width, unsigned int height,
    enum frame_format format, int planes_count,
    bool secure, bool pip)
{
  struct video_disp *disp = handle;
  struct drm_buf *gem_buf;
  struct drm_buf_metadata info;
  struct drm_frame* frame = calloc(1, sizeof(*frame));

  if (!frame) {
    GST_ERROR ("oom\n");
    return NULL;
  }

  memset(&info, 0 , sizeof(info));

  info.width = width;
  info.height = height;

  if (format == FRAME_FMT_NV21) {
    info.fourcc = DRM_FORMAT_NV21;
    info.flags = MESON_USE_VIDEO_PLANE;
  } else if (format == FRAME_FMT_NV12) {
    info.fourcc = DRM_FORMAT_NV12;
    info.flags = MESON_USE_VIDEO_PLANE;
  } else if (format == FRAME_FMT_AFBC) {
    info.fourcc = DRM_FORMAT_YUYV;
    info.flags = MESON_USE_VIDEO_AFBC;
  }

  if ((format == FRAME_FMT_NV21 || format == FRAME_FMT_NV12) && secure)
    info.flags |= MESON_USE_PROTECTED;

  if (!pip)
    info.flags |= MESON_USE_VD1;
  else
    info.flags |= MESON_USE_VD2;

  GST_LOG ("create buffer %dx%d fmt %d flag %x", width, height,
      info.fourcc, info.flags);

  gem_buf = drm_alloc_buf(disp->drm, &info);
  if (!gem_buf) {
    GST_ERROR ("Unable to alloc drm buf\n");
    goto error;
  }

  frame->buf = gem_buf;
  frame->pri_drm = handle;
  frame->destroy = frame_destroy;
  return frame;
error:
  if (frame) free (frame);
  return NULL;
}

int display_get_buffer_fds(struct drm_frame* drm_f, int *fd, int cnt)
{
  int i;
  struct drm_buf *gem_buf = drm_f->buf;

  if (gem_buf->nbo > cnt || !fd)
    return -1;

  for (i = 0; i < gem_buf->nbo; i++)
    fd[i] = gem_buf->fd[i];
  return 0;
}

void display_engine_stop(void *handle)
{
  struct video_disp* disp = handle;
  int rc;

  disp->started = false;
  if (disp->disp_t) {
    rc = pthread_join (disp->disp_t, NULL);
    if (rc)
      GST_ERROR ("join display thread %d", errno);
    disp->disp_t = 0;
  }

  pthread_mutex_lock (&disp->avsync_lock);
  if (disp->avsync) {
    av_sync_destroy (disp->avsync);
    disp->avsync = NULL;
  }
  pthread_mutex_unlock (&disp->avsync_lock);

  destroy_black_frame (disp->black_frame);
  drm_destroy_display (disp->drm);
  pthread_mutex_destroy (&disp->avsync_lock);
  free (disp);
}

static void * display_thread_func(void * arg)
{
  struct video_disp *disp = arg;
  struct drm_frame *f = NULL, *f_p1 = NULL, *f_p2 = NULL, *f_p3 = NULL;
  bool first_frame_rendered = false;
  drmVBlank vbl;

  prctl (PR_SET_NAME, "aml_v_dis");
  memset(&vbl, 0, sizeof(drmVBlank));

  while (disp->started) {
    int rc;
    struct drm_buf* gem_buf;
    struct vframe *sync_frame = NULL;

    vbl.request.type = DRM_VBLANK_RELATIVE;
    vbl.request.sequence = 1;
    vbl.request.signal = 0;

    if (first_frame_rendered) {
      rc = drmWaitVBlank(disp->drm->drm_fd, &vbl);
      if (rc) {
        GST_ERROR ("drmWaitVBlank error %d\n", rc);
        return NULL;
      }
    }

    pthread_mutex_lock (&disp->avsync_lock);
    if (disp->avsync)
      sync_frame = av_sync_pop_frame(disp->avsync);
    pthread_mutex_unlock (&disp->avsync_lock);
    if (!sync_frame)
      continue;

    f = sync_frame->private;

    if (!f) {
      disp->last_frame = true;
      break;
    }

    if (f != f_p1) {
      GST_LOG ("pop frame: %u", f->pts);
      gem_buf = f->buf;

      //set gem_buf window
      gem_buf->crtc_x = f->window.x;
      gem_buf->crtc_y = f->window.y;
      gem_buf->crtc_w = f->window.w;
      gem_buf->crtc_h = f->window.h;

      rc = drm_post_buf (disp->drm, gem_buf);
      if (rc) {
        GST_ERROR ("drm_post_buf error %d", rc);
        continue;
      }
      /* 2 fences delay on video layer, 1 fence delay on osd */
      if (f_p3) {
        f_p3->displayed = true;
        display_cb(disp->priv, f_p3->pri_dec, true);
        f_p3 = NULL;
      }

      f_p3 = f_p2;
      f_p2 = f_p1;
      f_p1 = f;
      first_frame_rendered = true;
    }
  }

  if (f_p3)
    display_cb(disp->priv, f_p3->pri_dec, false);
  if (f_p2)
    display_cb(disp->priv, f_p2->pri_dec, false);
  if (f_p1)
    display_cb(disp->priv, f_p1->pri_dec, false);

  GST_INFO ("quit %s\n", __func__);
  return NULL;
}

static void sync_frame_free(struct vframe * sync_frame)
{
  struct drm_frame* drm_f = sync_frame->private;
  struct video_disp *disp = drm_f->pri_drm;

  if (!disp) {
    GST_ERROR ("invalid arg");
    return;
  }

  if (drm_f) {
    drm_f->displayed = false;
    display_cb(disp->priv, drm_f->pri_dec, false);
  } else
    disp->last_frame = true;
}

int display_engine_show(void* handle, struct drm_frame* frame, struct rect* window)
{
  struct video_disp *disp = handle;
  int rc;
  struct vframe* sync_frame = &frame->sync_frame;

  if (!disp->avsync) {
    GST_ERROR ("avsync not started");
    return -1;
  }

  if (!disp->started) {
    disp->started = true;
    disp->last_frame = false;

    rc = pthread_create(&disp->disp_t, NULL, display_thread_func, disp);
    if (rc) {
      GST_ERROR ("create dispay thread fails\n");
      return -1;
    }
  }

  sync_frame->private = frame;
  sync_frame->pts = frame->pts;
  sync_frame->duration = frame->duration;
  sync_frame->free = sync_frame_free;
  frame->window = *window;

  while (disp->started) {
    if (av_sync_push_frame(disp->avsync, sync_frame)) {
      usleep(1000);
      continue;
    } else
      break;
  }
  GST_LOG ("push frame: %u", sync_frame->pts);

  return 0;
}

int display_engine_register_cb(displayed_cb_func cb)
{
  display_cb = cb;
  return 0;
}

int pause_pts_register_cb(pause_cb_func cb)
{
  pause_cb = cb;
  return 0;
}

int display_set_pause(void *handle, bool pause)
{
  struct video_disp *disp = handle;

  return av_sync_pause (disp->avsync, pause);
}

int display_set_pause_pts(void *handle, uint32_t pause_pts)
{
  struct video_disp *disp = handle;

  disp->pause_pts = pause_pts;
  return 0;
}

int display_show_black_frame(void * handle)
{
  struct video_disp *disp = handle;

  GST_INFO ("show black frame");
  return drm_post_buf (disp->drm, disp->black_frame->buf);
}

int display_set_speed(void *handle, float speed)
{
  struct video_disp *disp = handle;

  pthread_mutex_lock (&disp->avsync_lock);
  if (disp->avsync) {
    pthread_mutex_unlock (&disp->avsync_lock);
    return av_sync_set_speed (disp->avsync, speed);
  } else {
    disp->speed_pending = true;
    disp->speed = speed;
  }
  pthread_mutex_unlock (&disp->avsync_lock);

  return 0;
}
