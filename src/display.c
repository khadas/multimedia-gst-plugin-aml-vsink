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
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sched.h>
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
#include "aml_queue.h"
#include "display.h"

GST_DEBUG_CATEGORY_EXTERN(gst_aml_vsink_debug);
#define GST_CAT_DEFAULT gst_aml_vsink_debug

enum {
  BF_INVALID = 0,
  BF_WAIT_AV_SYNC,
  BF_WAIT_RENDER,
};

struct video_disp {
  struct drm_display *drm;
  bool last_frame;
  int drm_mode_set;
  void *priv;
  pthread_mutex_t avsync_lock;
  uint32_t pause_pts;
  bool check_underflow;
  struct drm_frame *black_frame;
  int black_frame_pending;
  struct drm_frame *cur_frame;


  /* trick play */
  bool speed_pending;
  float speed;

  /* avsync */
  void * avsync;
  int session;
  bool paused;

  /* low latency mode */
  bool low_latency;
  GQueue *fq;
  pthread_mutex_t fq_lock;

  /* display thread */
  bool disp_started;
  pthread_t disp_t;

  /* recycle thread */
  void * recycle_q;
  bool recycle_started;
  pthread_t recycle_t;

  /* scaling setting */
  GRWLock scale_lock;
  struct rect dst_win;
};

static struct drm_frame* create_black_frame (void* handle,
    unsigned int width, unsigned int height, bool pip);
static void destroy_black_frame (struct drm_frame *frame);
static int frame_destroy(struct drm_frame* drm_f);
static void * display_thread_func(void * arg);
static void * recycle_thread_func(void * arg);

displayed_cb_func display_cb;
pause_cb_func pause_cb;
underflow_cb_func underflow_cb;

static void display_res_change_cb(void *p)
{
  //struct video_disp *disp = p;
  //TODO
  return;
}

void *display_engine_start(void* priv, bool pip, bool low_latency)
{
  struct video_disp *disp = NULL;
  struct drm_display *drm;

  disp = (struct video_disp *)calloc (1, sizeof(*disp));
  if (!disp) {
    GST_ERROR ("oom");
    return NULL;
  }

  disp->recycle_q = create_q(32);
  if (!disp->recycle_q) {
    GST_ERROR ("recycle queue fail");
    goto error;
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
  disp->low_latency = low_latency;
  pthread_mutex_init (&disp->avsync_lock, NULL);
  pthread_mutex_init (&disp->fq_lock, NULL);

  disp->black_frame = create_black_frame (disp, 64, 64, pip);
  disp->black_frame_pending = BF_INVALID;
  disp->cur_frame = NULL;
  g_rw_lock_init (&disp->scale_lock);
  /* avsync log level */
  log_set_level(AVS_LOG_INFO);
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

static void underflow_check_cb(uint32_t pts, void* priv)
{
  struct video_disp * disp = priv;

  if (underflow_cb)
    underflow_cb (disp->priv, pts);
}

int display_start_avsync(void *handle, enum sync_mode mode, int id, int delay)
{
  struct video_disp * disp = handle;
  int ret = 0;

  disp->black_frame_pending = BF_INVALID;

  if (!disp->low_latency) {
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

    memset(&config, 0, sizeof(struct video_config));
    config.delay = 2;
    config.extra_delay = delay;
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
    if (disp->check_underflow) {
      GST_WARNING ("set check_underflow");
      av_sync_set_underflow_check_cb (disp->avsync, underflow_check_cb, disp, NULL);
    }
  } else {
    disp->fq = g_queue_new();
    if (!disp->fq) {
      GST_ERROR ("OOM\n");
      return -1;
    }
  }

  if (!disp->disp_started) {
    int rc;
    disp->disp_started = true;
    disp->last_frame = false;

    rc = pthread_create(&disp->disp_t, NULL, display_thread_func, disp);
    if (rc) {
      GST_ERROR ("create dispay thread fails\n");
      ret = -1;
    }

    disp->recycle_started = true;
    rc = pthread_create(&disp->recycle_t, NULL, recycle_thread_func, disp);
    if (rc) {
      GST_ERROR ("create recycle thread fails\n");
      return -1;
    }
  }

  return 0;

exit:
  pthread_mutex_unlock (&disp->avsync_lock);
  return ret;
}

void display_stop_avsync(void *handle)
{
  struct video_disp * disp = handle;

  if (disp->low_latency) {
    struct vframe *pop_frame;
    struct drm_frame *f;
    /* clean all frames */
    pthread_mutex_lock (&disp->fq_lock);
    do {
      pop_frame = g_queue_pop_head (disp->fq);
      if (pop_frame) {
        f = pop_frame->private;
        display_cb(disp->priv, f->pri_dec, true, true);
      }
    } while (pop_frame);
    pthread_mutex_unlock (&disp->fq_lock);
    GST_INFO ("clean frame queue");
    return;
  }

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
  if (disp->black_frame_pending == BF_WAIT_AV_SYNC) {
    disp->black_frame_pending = BF_WAIT_RENDER;
    GST_INFO ("show black frame stat: %d", disp->black_frame_pending);
  }
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
  uint32_t stride = (width + 63) & (~63);

  if (!frame) {
    GST_ERROR ("oom\n");
    return NULL;
  }

  memset(&info, 0 , sizeof(info));

  /* use single planar for black frame */
  info.width = width;
  info.height = height;
  info.flags = MESON_USE_VIDEO_PLANE;
  info.fourcc = DRM_FORMAT_NV12;

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
  frame->stride = stride;
  frame->height = height;
  frame->destroy = frame_destroy;

  frame->vaddr = mmap (NULL, stride * height, PROT_WRITE | PROT_READ,
      MAP_SHARED, gem_buf->fd[0], 0);
  if (frame->vaddr == MAP_FAILED) {
    GST_ERROR ("mmap fail %d", errno);
    drm_free_buf (gem_buf);
    goto error;
  }

  frame->vaddr1 = mmap (NULL, stride * height / 2, PROT_WRITE | PROT_READ,
      MAP_SHARED, gem_buf->fd[1], 0);
  if (frame->vaddr1 == MAP_FAILED) {
    GST_ERROR ("mmap fail %d", errno);
    munmap (frame->vaddr, stride * height);
    drm_free_buf (gem_buf);
    goto error;
  }

  /* full screen black frame */
  memset(frame->vaddr, 0, stride * height);
  memset(frame->vaddr1, 0x80, stride * height / 2);

  gem_buf->src_x = 0;
  gem_buf->src_y = 0;
  gem_buf->src_w = width;
  gem_buf->src_h = height;

  gem_buf->crtc_x = 0;
  gem_buf->crtc_y = 0;
  gem_buf->crtc_w = 0;
  gem_buf->crtc_h = 0;

  return frame;
error:
  if (frame) free (frame);
  return NULL;
}

static void destroy_black_frame (struct drm_frame *frame)
{
  if (!frame)
    return;

  munmap (frame->vaddr, frame->stride * frame->height);
  munmap (frame->vaddr1, frame->stride * frame->height / 2);
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

  disp->disp_started = false;
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

  pthread_mutex_lock(&disp->fq_lock);
  if (disp->fq) {
    g_queue_free(disp->fq);
    disp->fq = NULL;
  }
  pthread_mutex_unlock(&disp->fq_lock);

  disp->recycle_started = false;
  if (disp->recycle_t) {
    rc = pthread_join (disp->recycle_t, NULL);
    if (rc)
      GST_ERROR ("join recycle thread %d", errno);
    disp->recycle_t = 0;
  }

  if (disp->recycle_q) {
    destroy_q(disp->recycle_q);
    disp->recycle_q = NULL;
  }
  destroy_black_frame (disp->black_frame);
  drm_destroy_display (disp->drm);
  g_rw_lock_clear (&disp->scale_lock);
  disp->drm = NULL;
  pthread_mutex_destroy (&disp->avsync_lock);
  free (disp);
}

/* Should only call during pause state
 * It will update the current frame position
 */
void display_engine_refresh(void* handle, struct rect *dst, struct rect *src)
{
  struct video_disp* disp = handle;

  pthread_mutex_lock (&disp->avsync_lock);
  if (disp->paused && disp->drm && disp->cur_frame) {
    struct drm_buf* gem_buf;

    gem_buf = disp->cur_frame->buf;
    gem_buf->crtc_x = dst->x;
    gem_buf->crtc_y = dst->y;
    gem_buf->crtc_w = dst->w;
    gem_buf->crtc_h = dst->h;
    gem_buf->src_x = src->x;
    gem_buf->src_y = src->y;
    gem_buf->src_w = src->w;
    gem_buf->src_h = src->h;

    disp->drm->set_plane(disp->drm, gem_buf);
  }
  pthread_mutex_unlock (&disp->avsync_lock);
}

static void * display_thread_func(void * arg)
{
  struct video_disp *disp = arg;
  struct drm_frame *f = NULL, *f_old = NULL;
  bool first_frame_rendered = false;
  drmVBlank vbl;
  struct sched_param schedParam;
  int lastVsync_Cnt = -1;

  GST_DEBUG ("enter");
  prctl (PR_SET_NAME, "aml_v_dis");
  memset(&vbl, 0, sizeof(drmVBlank));

  schedParam.sched_priority = sched_get_priority_max(SCHED_FIFO);
  if (pthread_setschedparam (pthread_self(), SCHED_FIFO, &schedParam))
    GST_WARNING ("fail to set display_thread_func priority");

  {
    int j;
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    for (j = 0; j < 2; j++)
      CPU_SET(j, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset))
      GST_WARNING ("fail to set cpu affinity");
  }

  while (disp->disp_started) {
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
    } else {
      usleep(1000);
    }

    if (!disp->low_latency) {
      pthread_mutex_lock (&disp->avsync_lock);
      if (disp->avsync)
        sync_frame = av_sync_pop_frame(disp->avsync);
      pthread_mutex_unlock (&disp->avsync_lock);
    } else {
      struct vframe *pop_frame;
      struct vframe *pre_frame = NULL;

      /* get the latest frame */
      pthread_mutex_lock (&disp->fq_lock);
      do {
        pop_frame = g_queue_pop_head (disp->fq);

        /* release preframe */
        if (pre_frame && pop_frame) {
          struct drm_frame* f = pre_frame->private;
          display_cb(disp->priv, f->pri_dec, true, false);
        }
        pre_frame = pop_frame;

        if (pop_frame)
          sync_frame = pop_frame;
      } while (pop_frame);
      pthread_mutex_unlock (&disp->fq_lock);
    }

    /* handle black frame here so black frame is also inserted
     * after all valid frames */
    if (!sync_frame) {
      lastVsync_Cnt++;
      if (disp->black_frame_pending == BF_WAIT_RENDER) {
        drm_post_buf (disp->drm, disp->black_frame->buf);
        disp->black_frame_pending = BF_INVALID;
        lastVsync_Cnt = 0;
        GST_INFO ("show black frame stat: 3");
      }
      usleep(1000);
      continue;
    }

    if (!first_frame_rendered)
      log_info("vsink rendering first ts");

    f = sync_frame->private;

    if (!f) {
      disp->last_frame = true;
      break;
    }

    if (f != f_old) {
      GST_LOG ("pop frame: %u", f->pts);
      gem_buf = f->buf;

      //set gem_buf window
      g_rw_lock_reader_lock (&disp->scale_lock);
      gem_buf->crtc_x = disp->dst_win.x;
      gem_buf->crtc_y = disp->dst_win.y;
      gem_buf->crtc_w = disp->dst_win.w;
      gem_buf->crtc_h = disp->dst_win.h;
      g_rw_lock_reader_unlock (&disp->scale_lock);

      gem_buf->src_x = f->source_window.x;
      gem_buf->src_y = f->source_window.y;
      gem_buf->src_w = f->source_window.w;
      gem_buf->src_h = f->source_window.h;

      rc = drm_post_buf (disp->drm, gem_buf);
      if (rc)
        GST_ERROR ("drm_post_buf errno %d", errno);

      /* when next two frame are posted, fence can be retrieved.
       * So introduce two frames delay here
       */
      if (f_old) {
        rc = queue_item (disp->recycle_q, f_old);
        if (rc) {
          GST_ERROR ("queue fail %d qlen %d", rc, queue_size(disp->recycle_q));
          display_cb(disp->priv, f_old->pri_dec, true, false);
        }
      }

      f_old = f;
      disp->cur_frame = f;
      first_frame_rendered = true;
      lastVsync_Cnt = 0;
    }
  }

  if (lastVsync_Cnt != -1 && lastVsync_Cnt < 2) {
    GST_LOG ("wait about 2 vsync for last frame");
    usleep(40000);
  }

  if (f_old)
     display_cb(disp->priv, f_old->pri_dec, true, true);

  GST_INFO ("quit %s", __func__);
  return NULL;
}

static void * recycle_thread_func(void * arg)
{
  struct video_disp *disp = arg;
  struct drm_buf* gem_buf;
  struct drm_frame *f = NULL;
  int rc;
  struct sched_param schedParam;

  prctl (PR_SET_NAME, "aml_v_recy");

  schedParam.sched_priority = sched_get_priority_max(SCHED_FIFO) / 2;
  if (pthread_setschedparam (pthread_self(), SCHED_FIFO, &schedParam))
    GST_WARNING ("fail to set recycle_thread_func priority");

  while (disp->recycle_started) {
    if (dqueue_item(disp->recycle_q, (void **)&f)) {
      usleep(5000);
      continue;
    }
    if (!f) {
      usleep(5000);
      continue;
    }
    gem_buf = f->buf;
    rc = drm_waitvideoFence(gem_buf->fd[0]);
    if (rc <= 0)
      GST_WARNING ("wait fence error %d", rc);
    display_cb(disp->priv, f->pri_dec, true, false);
  }

  while (!dqueue_item(disp->recycle_q, (void **)&f)) {
    display_cb(disp->priv, f->pri_dec, false, true);
  }

  GST_INFO ("quit %s", __func__);
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
    display_cb(disp->priv, drm_f->pri_dec, false, false);
  } else {
    disp->last_frame = true;
    GST_INFO ("last frame detected");
  }
}

int display_engine_show(void* handle, struct drm_frame* frame, struct rect* src_window)
{
  struct video_disp *disp = handle;
  int rc;
  struct vframe* sync_frame = &frame->sync_frame;

  if (!disp || (!disp->avsync && !disp->low_latency) || (!disp->fq && disp->low_latency)) {
    GST_ERROR ("avsync not started");
    return -1;
  }
  if (!src_window) {
    GST_ERROR ("invalid window pointer");
    return -1;
  }

  sync_frame->private = frame;
  sync_frame->pts = frame->pts;
  sync_frame->duration = frame->duration;
  sync_frame->free = sync_frame_free;
  frame->source_window = *src_window;

  if (!disp->low_latency) {
    rc = av_sync_push_frame(disp->avsync, sync_frame);
    if (!rc)
      GST_LOG ("push frame: %u", sync_frame->pts);
  } else {
    pthread_mutex_lock (&disp->fq_lock);
    g_queue_push_tail(disp->fq, sync_frame);
    pthread_mutex_unlock (&disp->fq_lock);
  }

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

int display_underflow_register_cb(underflow_cb_func cb)
{
  underflow_cb = cb;
  return 0;
}
int display_set_checkunderflow(void *handle, bool underflow_check)
{
  struct video_disp *disp = handle;

  disp->check_underflow = underflow_check;
  return 0;
}

int display_set_pause(void *handle, bool pause)
{
  struct video_disp *disp = handle;
  int rc = 0;

  GST_INFO ("pause %d", pause);
  if (!disp->low_latency) {
    /* don't need avsync_lock here, because object lock will be used
     * for race condition
     */
    if (disp->avsync) {
      rc = av_sync_pause (disp->avsync, pause);
      disp->paused = pause;
    }
  }
  return rc;
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

  pthread_mutex_lock (&disp->avsync_lock);
  if (disp->avsync)
    disp->black_frame_pending = BF_WAIT_AV_SYNC;
  else
    disp->black_frame_pending = BF_WAIT_RENDER;
  GST_INFO ("show black frame stat: %d", disp->black_frame_pending);
  pthread_mutex_unlock (&disp->avsync_lock);
}

int display_set_speed(void *handle, float speed)
{
  struct video_disp *disp = handle;

  if (disp->low_latency)
    return 0;

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

void display_engine_set_dst_rect(void *handle, struct rect *window)
{
  struct video_disp *disp = handle;

  if (memcmp(&disp->dst_win, window, sizeof(*window))) {
    g_rw_lock_writer_lock (&disp->scale_lock);
    memcpy (&disp->dst_win, window, sizeof(*window));
    g_rw_lock_writer_unlock (&disp->scale_lock);
  }
}

void display_set_video_delay(void *handle, int delay_ms)
{
  struct video_disp *disp = handle;
  struct video_config config;

  if (!disp)
    return;

  pthread_mutex_lock (&disp->avsync_lock);
  if (disp->avsync) {
    memset(&config, 0, sizeof(struct video_config));
    config.delay = 2;
    config.extra_delay = delay_ms;
    av_sync_video_config(disp->avsync, &config);
  }
  pthread_mutex_unlock (&disp->avsync_lock);
}
