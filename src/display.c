/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
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
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define MAX_BUF_SIZE 32

static char* drm_master_dev_name = "/dev/dri/card0";
static char* drm_cli_dev_name = "/dev/dri/renderD128";
extern char mode_str[16];
extern unsigned int vfresh;

struct gem_buffer {
  uint32_t width;
  uint32_t height;
  unsigned int planes_count;
  unsigned int size;
  unsigned int handles[4];
  unsigned int pitches[4];
  unsigned int offsets[4];
  int export_fds[4];

  unsigned int framebuffer_id;
};

struct display_properties_ids {
  uint32_t connector_crtc_id;
  uint32_t crtc_mode_id;
  uint32_t crtc_active;
  uint32_t plane_fb_id;
  uint32_t plane_crtc_id;
  uint32_t plane_src_x;
  uint32_t plane_src_y;
  uint32_t plane_src_w;
  uint32_t plane_src_h;
  uint32_t plane_crtc_x;
  uint32_t plane_crtc_y;
  uint32_t plane_crtc_w;
  uint32_t plane_crtc_h;
  uint32_t plane_zpos;
};

struct display_setup {
  unsigned int connector_id;
  unsigned int encoder_id;
  unsigned int crtc_id;
  unsigned int plane_id;

  unsigned int x;
  unsigned int y;
  unsigned int scaled_width;
  unsigned int scaled_height;
  unsigned int crtc_width;
  unsigned int crtc_height;

  struct display_properties_ids properties_ids;
  drmModeModeInfo mode;

  struct plane *plane;
  struct crtc *crtc;
  struct connector *connector;
};

struct video_disp {
  int drm_fd;
  int drm_cli_fd;
  struct display_setup setup;
  struct gem_buffer osd_gem_buf;

  bool started;
  pthread_t disp_t;
  void * avsync;
  bool last_frame;
  int drm_mode_set;
  void *priv;
};

struct plane {
  drmModePlane *plane;
  drmModeObjectProperties *props;
  drmModePropertyRes **props_info;
};

struct crtc {
  drmModeCrtc *crtc;
  drmModeObjectProperties *props;
  drmModePropertyRes **props_info;
};

struct connector {
  drmModeConnector *connector;
  drmModeObjectProperties *props;
  drmModePropertyRes **props_info;
};

static struct gem_buffer *gem_buf;
displayed_cb_func display_cb;

static int OsdBufferCreate(int fd, uint32_t width,
        uint32_t height, struct gem_buffer *buffer);

static int create_meson_gem_buffer(int fd, enum frame_format fmt,
        struct gem_buffer *buffer, bool secure)
{
    struct drm_meson_gem_create gem_create;
    int rc;
    int i;
    int width = buffer->width;
    int height = buffer->height;

    if (!buffer)
        return -1;

    for (i = 0; i < buffer->planes_count; i++) {
        memset(&gem_create, 0, sizeof(gem_create));

        if (fmt == FRAME_FMT_AFBC) {
            gem_create.flags = MESON_USE_VIDEO_AFBC;
            gem_create.size = width * height * 2;
            buffer->pitches[i] = width*2;
        } else {
            gem_create.flags = MESON_USE_VIDEO_PLANE;
            if (secure)
                 gem_create.flags |= MESON_USE_PROTECTED;
            if (i == 0)
                gem_create.size = width * height;
            else
                gem_create.size = width * height / 2;
            buffer->pitches[i] = width;
        }

        rc = drmIoctl(fd, DRM_IOCTL_MESON_GEM_CREATE, &gem_create);
        if (rc < 0) {
            printf("Unable to create dumb buffer: %s\n",
                    strerror(errno));
            return -1;
        }

        buffer->size += gem_create.size;
        buffer->handles[i] = gem_create.handle;
        buffer->offsets[i] = 0;

        rc = drmPrimeHandleToFD(fd, buffer->handles[i], DRM_CLOEXEC | DRM_RDWR,
                &buffer->export_fds[i]);
        if (rc < 0) {
            fprintf(stderr, "drmPirmeHandleToFD fail: %s\n", strerror(errno));
            return -1;
        }
    }

    return 0;
}

static int close_buffer(int fd, struct gem_buffer *buffer)
{
    int rc;
    int i;
    struct drm_mode_destroy_dumb destroy_dumb;

    if (!buffer)
        return -1;

    for (i = 0; i < buffer->planes_count; i++)
        close(buffer->export_fds[i]);

    drmModeRmFB(fd, buffer->framebuffer_id);

    memset(&destroy_dumb, 0, sizeof(destroy_dumb));
    for (i = 0; i < buffer->planes_count; i++) {
        destroy_dumb.handle = buffer->handles[i];

        rc = drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);
        if (rc < 0) {
            /* If handle was from drmPrimeFDToHandle, then fd is connected
             * as render, we have to use drm_gem_close to release it.
             */
            if (errno == EACCES) {
                struct drm_gem_close close_req;
                close_req.handle = destroy_dumb.handle;
                rc = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_req);
                if (rc < 0) {
                    fprintf(stderr, "Unable to destroy buffer: %s\n",
                            strerror(errno));
                    return -1;
                }
            }
        }
    }

    return 0;
}

static int add_framebuffer(int fd, struct gem_buffer *buffer,
        enum frame_format fmt, uint64_t modifier)
{
    uint64_t modifiers[4] = { 0, 0, 0, 0 };
    uint32_t flags = 0;
    unsigned int id;
    unsigned int i;
    int rc;
    uint32_t drm_fmt;

    if (fmt == FRAME_FMT_NV21)
        drm_fmt = DRM_FORMAT_NV21;
    else if (fmt == FRAME_FMT_NV12)
        drm_fmt = DRM_FORMAT_NV12;
    else if (fmt == FRAME_FMT_AFBC)
        drm_fmt = DRM_FORMAT_YUYV;
    else {
        printf("ftm %d not supported\n", fmt);
        return -1;
    }

    for (i = 0; i < buffer->planes_count; i++) {
        if (buffer->handles[i] != 0 &&
                modifier != DRM_FORMAT_MOD_NONE) {
            flags |= DRM_MODE_FB_MODIFIERS;
            modifiers[i] = modifier;
        }
    }

    rc = drmModeAddFB2WithModifiers(fd, buffer->width,
            buffer->height, drm_fmt,
            buffer->handles, buffer->pitches,
            buffer->offsets, modifiers, &id, flags);
    if (rc < 0) {
        fprintf(stderr, "Unable to add framebuffer for plane: %s\n",
                strerror(errno));
        return -1;
    }

    buffer->framebuffer_id = id;

    return 0;
}

static int discover_properties(struct video_disp *disp, struct display_properties_ids *ids)
{
    //int connector_id = setup.connector_id;
    int fd = disp->drm_fd;
    int plane_id = disp->setup.plane_id;
    drmModeObjectPropertiesPtr properties = NULL;
    drmModePropertyPtr property = NULL;
    struct {
        uint32_t object_type;
        uint32_t object_id;
        char *name;
        uint32_t *value;
    } glue[] = {
        { DRM_MODE_OBJECT_PLANE, plane_id, "FB_ID", &ids->plane_fb_id },
        { DRM_MODE_OBJECT_PLANE, plane_id, "CRTC_ID", &ids->plane_crtc_id },
        { DRM_MODE_OBJECT_PLANE, plane_id, "SRC_X", &ids->plane_src_x },
        { DRM_MODE_OBJECT_PLANE, plane_id, "SRC_Y", &ids->plane_src_y },
        { DRM_MODE_OBJECT_PLANE, plane_id, "SRC_W", &ids->plane_src_w },
        { DRM_MODE_OBJECT_PLANE, plane_id, "SRC_H", &ids->plane_src_h },
        { DRM_MODE_OBJECT_PLANE, plane_id, "CRTC_X", &ids->plane_crtc_x },
        { DRM_MODE_OBJECT_PLANE, plane_id, "CRTC_Y", &ids->plane_crtc_y },
        { DRM_MODE_OBJECT_PLANE, plane_id, "CRTC_W", &ids->plane_crtc_w },
        { DRM_MODE_OBJECT_PLANE, plane_id, "CRTC_H", &ids->plane_crtc_h },
    };
    unsigned int i, j;
    int rc;

    for (i = 0; i < ARRAY_SIZE(glue); i++) {
        properties = drmModeObjectGetProperties(fd,
                glue[i].object_id,
                glue[i].object_type);
        if (properties == NULL) {
            fprintf(stderr, "Unable to get DRM properties(%s): %s\n",
                    glue[i].name, strerror(errno));
            goto error;
        }

        for (j = 0; j < properties->count_props; j++) {
            property = drmModeGetProperty(fd,
                    properties->props[j]);
            if (property == NULL) {
                fprintf(stderr, "Unable to get DRM property: %s\n",
                        strerror(errno));
                goto error;
            }

            if (strcmp(property->name, glue[i].name) == 0) {
                *glue[i].value = property->prop_id;
                break;
            }

            drmModeFreeProperty(property);
            property = NULL;
        }

        if (j == properties->count_props) {
            fprintf(stderr, "Unable to find property for %s\n",
                    glue[i].name);
            goto error;
        }

        drmModeFreeProperty(property);
        property = NULL;

        drmModeFreeObjectProperties(properties);
        properties = NULL;
    }

    rc = 0;
    goto complete;

error:
    rc = -1;

complete:
    if (property != NULL)
        drmModeFreeProperty(property);

    if (properties != NULL)
        drmModeFreeObjectProperties(properties);

    return rc;
}

static int add_connector_property(drmModeAtomicReq *req, uint32_t obj_id,
        const char *name, uint64_t value, struct display_setup *setup)
{
    struct connector *obj = setup->connector;
    unsigned int i;
    int prop_id = 0;

    for (i = 0 ; i < obj->props->count_props ; i++) {
        if (strcmp(obj->props_info[i]->name, name) == 0) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id < 0) {
        printf("no connector property: %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

static int add_crtc_property(drmModeAtomicReq *req, uint32_t obj_id,
        const char *name, uint64_t value, struct display_setup *setup)
{
    struct crtc *obj = setup->crtc;
    unsigned int i;
    int prop_id = -1;

    for (i = 0 ; i < obj->props->count_props ; i++) {
        if (strcmp(obj->props_info[i]->name, name) == 0) {
            prop_id = obj->props_info[i]->prop_id;
            break;
        }
    }

    if (prop_id < 0) {
        printf("no crtc property: %s\n", name);
        return -EINVAL;
    }

    return drmModeAtomicAddProperty(req, obj_id, prop_id, value);
}

int span(struct timeval* t1, struct timeval* t2)
{
    int64_t time = (t2->tv_sec - t1->tv_sec);
    time *= 1000000;
    time += t2->tv_usec - t1->tv_usec;
    time /= 1000;
    return (int)time;
}

static int page_flip(struct video_disp *disp, struct gem_buffer* gem_buf)
{
  int fd = disp->drm_fd;
  unsigned int crtc_id = disp->setup.crtc_id;
  unsigned int plane_id = disp->setup.plane_id;
  struct display_properties_ids *ids = &disp->setup.properties_ids;
  drmModeAtomicReqPtr request;
  uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;
  int rc;

  request = drmModeAtomicAlloc();

  if (!disp->drm_mode_set) {
    unsigned int connector_id = disp->setup.connector_id;
    uint32_t blob_id;

    flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (add_connector_property(request, connector_id, "CRTC_ID",
          crtc_id, &disp->setup) < 0)
      return -1;

    if (drmModeCreatePropertyBlob(fd, &disp->setup.mode, sizeof(drmModeModeInfo),
          &blob_id) != 0)
      return -1;

    if (add_crtc_property(request, crtc_id, "MODE_ID", blob_id, &disp->setup) < 0)
      return -1;

    if (add_crtc_property(request, crtc_id, "ACTIVE", 1, &disp->setup) < 0)
      return -1;

    drmModeAtomicAddProperty(request, plane_id, ids->plane_crtc_x, 0);
    drmModeAtomicAddProperty(request, plane_id, ids->plane_crtc_y, 0);
    drmModeAtomicAddProperty(request, plane_id, ids->plane_crtc_w,
        disp->setup.scaled_width);
    drmModeAtomicAddProperty(request, plane_id, ids->plane_crtc_h,
        disp->setup.scaled_height);
    disp->drm_mode_set = 1;
  }

  drmModeAtomicAddProperty(request, plane_id, ids->plane_src_x, 0);
  drmModeAtomicAddProperty(request, plane_id, ids->plane_src_y, 0);
  drmModeAtomicAddProperty(request, plane_id, ids->plane_src_w,
      gem_buf->width << 16);
  drmModeAtomicAddProperty(request, plane_id, ids->plane_src_h,
      gem_buf->height << 16);

  drmModeAtomicAddProperty(request, plane_id, ids->plane_fb_id,
      gem_buf->framebuffer_id);
  drmModeAtomicAddProperty(request, plane_id, ids->plane_crtc_id,
      crtc_id);

  rc = drmModeAtomicCommit(fd, request, flags, NULL);
  if (rc < 0) {
    fprintf(stderr, "Unable to flip page: %s\n", strerror(errno));
    goto error;
  }

  rc = 0;
  goto complete;

error:
  rc = -1;

complete:
  drmModeAtomicFree(request);

  return rc;
}

static uint32_t find_crtc_for_encoder(const drmModeRes *resources,
        const drmModeEncoder *encoder) {
    int i;

    for (i = 0; i < resources->count_crtcs; i++) {
        /* possible_crtcs is a bitmask as described here:
         * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
         */
        const uint32_t crtc_mask = 1 << i;
        const uint32_t crtc_id = resources->crtcs[i];
        if (encoder->possible_crtcs & crtc_mask) {
            return crtc_id;
        }
    }

    /* no match found */
    return -1;
}

static uint32_t find_crtc_for_connector(int fd, const drmModeRes *resources,
        const drmModeConnector *connector) {
    int i;

    for (i = 0; i < connector->count_encoders; i++) {
        const uint32_t encoder_id = connector->encoders[i];
        drmModeEncoder *encoder = drmModeGetEncoder(fd, encoder_id);

        if (encoder) {
            const uint32_t crtc_id = find_crtc_for_encoder(resources, encoder);

            drmModeFreeEncoder(encoder);
            if (crtc_id != 0) {
                return crtc_id;
            }
        }
    }

    /* no match found */
    return -1;
}

static int init_drm(struct video_disp *disp)
{
  int fd = disp->drm_fd;
  drmModeRes *resources;
  drmModeConnector *connector = NULL;
  drmModeEncoder *encoder = NULL;
  int i, area, rc;
  drmModeModeInfo *curr_mode = NULL;

  resources = drmModeGetResources(fd);
  if (!resources) {
    GST_ERROR ("drmModeGetResources failed: %s", strerror(errno));
    return -1;
  }

  /* find a connected connector: */
  for (i = 0; i < resources->count_connectors; i++) {
    connector = drmModeGetConnector(fd, resources->connectors[i]);
    if (connector->connection == DRM_MODE_CONNECTED) {
      /* it's connected, let's use this! */
      break;
    }
    drmModeFreeConnector(connector);
    connector = NULL;
  }

  if (!connector) {
    GST_ERROR ("no connected connector!");
    return -1;
  }

  disp->setup.connector_id = connector->connector_id;

  /* find preferred mode or the highest resolution mode: */

  if (*mode_str) {
    for (i = 0; i < connector->count_modes; i++) {
      drmModeModeInfo *current_mode = &connector->modes[i];

      if (current_mode->name && strcmp(current_mode->name, mode_str) == 0) {
        if (vfresh == 0 || current_mode->vrefresh == vfresh) {
          curr_mode = current_mode;
          printf("found the request mode: %s-%d.\n", current_mode->name,
              current_mode->vrefresh);
          break;
        }
      }
    }
  }

  if (!curr_mode) {
    printf("requested mode not found, using default mode!\n");
    for (i = 0, area = 0; i < connector->count_modes; i++) {
      drmModeModeInfo *current_mode = &connector->modes[i];

      if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
        curr_mode = current_mode;
        break;
      }

      int current_area = current_mode->hdisplay * current_mode->vdisplay;
      if (current_area > area) {
        curr_mode = current_mode;
        area = current_area;
      }
    }
  }

  if (!curr_mode) {
    printf("could not find mode!\n");
    return -1;
  }

  memcpy(&disp->setup.mode, curr_mode, sizeof(drmModeModeInfo));

  /* find encoder: */
  for (i = 0; i < resources->count_encoders; i++) {
    encoder = drmModeGetEncoder(fd, resources->encoders[i]);
    if (encoder->encoder_id == connector->encoder_id)
      break;
    drmModeFreeEncoder(encoder);
    encoder = NULL;
  }

  if (encoder) {
    disp->setup.crtc_id = encoder->crtc_id;
  } else {
    uint32_t crtc_id1 = find_crtc_for_connector(fd, resources, connector);

    disp->setup.crtc_id = crtc_id1;
  }

  drmModeFreeResources(resources);

  /* default set to full sreen */
  disp->setup.crtc_width = disp->setup.mode.hdisplay;
  disp->setup.crtc_height = disp->setup.mode.vdisplay;
  disp->setup.scaled_width = disp->setup.mode.hdisplay;
  disp->setup.scaled_height = disp->setup.mode.vdisplay;

  //TODO: runtime dectection
  disp->setup.plane_id = 28;

  GST_INFO ("plane: %u, crtc: %u, encoder:%u, connector: %u",
      disp->setup.plane_id, disp->setup.crtc_id,
      disp->setup.encoder_id, disp->setup.connector_id);

  rc = discover_properties(disp, &disp->setup.properties_ids);
  if (rc < 0) {
    fprintf(stderr, "Unable to discover DRM properties\n");
    return -1;
  }

  disp->setup.plane = calloc(1, sizeof(struct plane));
  disp->setup.crtc = calloc(1, sizeof(struct crtc));
  disp->setup.connector = calloc(1, sizeof(struct connector));

#define get_resource(type, Type, id) do { 					\
  disp->setup.type->type = drmModeGet##Type(disp->drm_fd, id);			\
  if (!disp->setup.type->type) {						\
    GST_ERROR("could not get %s %i: %s", #type, id, strerror(errno));		\
    return -1;						\
  }								\
} while (0)

  get_resource(plane, Plane, disp->setup.plane_id);
  get_resource(crtc, Crtc, disp->setup.crtc_id);
  get_resource(connector, Connector, disp->setup.connector_id);

#define get_properties(type, TYPE, id) do {					\
  uint32_t i;							\
  disp->setup.type->props = drmModeObjectGetProperties(disp->drm_fd,		\
      id, DRM_MODE_OBJECT_##TYPE);			\
  if (!disp->setup.type->props) {						\
    GST_ERROR ("could not get %s %u properties: %s",#type, id, strerror(errno));		\
    return -1;						\
  }								\
  disp->setup.type->props_info = calloc(disp->setup.type->props->count_props,	\
      sizeof(disp->setup.type->props_info));			\
  for (i = 0; i < disp->setup.type->props->count_props; i++) {		\
    disp->setup.type->props_info[i] = drmModeGetProperty(disp->drm_fd,	\
        disp->setup.type->props->props[i]);		\
  }								\
} while (0)

  get_properties(plane, PLANE, disp->setup.plane_id);
  get_properties(crtc, CRTC, disp->setup.crtc_id);
  get_properties(connector, CONNECTOR, disp->setup.connector_id);

  /* make osd transparent */
  rc = OsdBufferCreate(disp->drm_cli_fd, disp->setup.crtc_width,
      disp->setup.crtc_height, &disp->osd_gem_buf);
  if (rc) {
    GST_ERROR ("OsdBufferCreate fail %d", rc);
    return -1;
  }
  rc = drmModeSetCrtc(disp->drm_fd, disp->setup.crtc_id,
      disp->osd_gem_buf.framebuffer_id, 0, 0,
      &disp->setup.connector_id, 1, &disp->setup.mode);
  if (rc) {
    close_buffer(disp->drm_cli_fd, &disp->osd_gem_buf);
    GST_ERROR ("drmModeSetCrtc fail %d\n", rc);
    return -1;
  }

  return 0;
}

static int OsdBufferCreate(int fd, uint32_t width,
        uint32_t height, struct gem_buffer *buffer) {
    int rc;
    uint32_t flags = 0;
    void *data;
    struct drm_mode_create_dumb create_dumb;
    uint64_t modifiers[4] = { 0, 0, 0, 0 };
    struct drm_mode_map_dumb map_dumb;

    memset(&create_dumb, 0, sizeof(create_dumb));
    create_dumb.width = width;
    create_dumb.height = height;
    create_dumb.bpp = 32;

    rc = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
    if (rc < 0) {
        printf("Unable to create dumb buffer: %s\n", strerror(errno));
        return -1;
    }

    buffer->size = create_dumb.size;
    buffer->pitches[0] = create_dumb.pitch;
    buffer->offsets[0] = 0;
    buffer->handles[0] = create_dumb.handle;

    rc = drmModeAddFB2WithModifiers(fd, width, height, DRM_FORMAT_ARGB8888,
            buffer->handles, buffer->pitches,
            buffer->offsets, modifiers, &buffer->framebuffer_id, flags);
    if (rc < 0) {
        printf("Unable to add framebuffer for osd plane: %s\n", strerror(errno));
        return -1;
    }

    memset(&map_dumb, 0, sizeof(map_dumb));
    map_dumb.handle = buffer->handles[0];
    rc = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb);
    if (rc < 0) {
        printf("Unable to map buffer: %s\n", strerror(errno));
        return -1;
    }

    data = mmap(0, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
            map_dumb.offset);
    if (data == MAP_FAILED) {
        printf("Unable to mmap buffer: %s\n", strerror(errno));
        return -1;
    }

    /* make it transparent */
    memset(data, 0, buffer->size);

    munmap(data, buffer->size);
    return 0;
}

void *display_engine_start(void* priv)
{
  int rc;
  struct video_disp *disp;

  disp = (struct video_disp *)calloc (1, sizeof(*disp));
  if (!disp) {
    GST_ERROR ("oom");
  }
  disp->drm_fd = drmOpen("meson", drm_master_dev_name);
  if (disp->drm_fd < 0) {
    GST_ERROR ("Unable to open DRM node: %s", strerror(errno));
    goto error;
  }

  disp->drm_cli_fd = drmOpen("meson", drm_cli_dev_name);
  if (disp->drm_cli_fd < 0) {
    GST_ERROR ("Unable to open client DRM node: %s", strerror(errno));
    goto error;
  }

  rc = drmSetClientCap(disp->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);
  if (rc < 0) {
    GST_ERROR ("Unable to set DRM atomic capability: %s\n",
        strerror(errno));
    goto error;
  }

  rc = drmSetClientCap(disp->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (rc < 0) {
    GST_ERROR ("Unable to set DRM universal planes capability: %s\n",
        strerror(errno));
    goto error;
  }

  /* prepare all connectors and CRTCs */
  rc = init_drm(disp);
  if (rc) {
    GST_ERROR ("modeset_prepare fail");
    goto error;
  }
  disp->priv = priv;

  /* avsync log level */
  log_set_level(LOG_INFO);
  return disp;
error:
  free (disp);
  return NULL;
}

//TODO
int display_set_avsync_mode(void *handle, enum sync_mode mode)
{
  struct video_disp * disp = handle;

  disp->avsync = av_sync_create(0, AV_SYNC_MODE_VMASTER, 2, 2, 90000/disp->setup.mode.vrefresh);
  if (!disp->avsync) {
    printf("create avsync fails\n");
    return -1;
  }
  return 0;
}

static int frame_destroy(struct drm_frame* drm_f)
{
    int rc;
    struct gem_buffer *gem_buf = drm_f->gem;
    struct video_disp * disp = drm_f->pri_drm;

    rc = close_buffer(disp->drm_cli_fd, gem_buf);
    free(gem_buf);
    free(drm_f);
    return rc;
}

struct drm_frame* display_create_buffer(void* handle,
    unsigned int width, unsigned int height,
    enum frame_format format, int planes_count, bool secure)
{
    int rc;
    struct video_disp * disp = handle;
    struct drm_frame* frame = calloc(1, sizeof(*frame));
    struct gem_buffer *gem_buf = calloc(1, sizeof(*gem_buf));

    if (!frame || !gem_buf) {
        printf("oom\n");
        return NULL;
    }

    gem_buf->width = width;
    gem_buf->height = height;
    frame->gem = gem_buf;
    frame->destroy = frame_destroy;
    frame->pri_drm = handle;

    gem_buf->planes_count = planes_count;

    rc = create_meson_gem_buffer(disp->drm_cli_fd, format, gem_buf, secure);
    if (rc < 0) {
        printf("create_meson_gem_buffer fail %d\n", rc);
        goto error;
    }
    if (planes_count == 2)
        printf("export_fd: %d/%d\n",
                gem_buf->export_fds[0],
                gem_buf->export_fds[1]);
    else
        printf("export_fd: %d\n",
                gem_buf->export_fds[0]);

    rc = add_framebuffer(disp->drm_cli_fd, gem_buf,
            format, DRM_FORMAT_MOD_NONE);
    if (rc < 0) {
        printf("Unable to add DRM framebuffer\n");
        goto error;
    }

    return frame;
error:
    close_buffer(disp->drm_cli_fd, gem_buf);
    if (frame) free(frame);
    if (gem_buf) free(gem_buf);
    return NULL;
}

int display_get_buffer_fds(struct drm_frame* drm_f, int *fd, int cnt)
{
    int i;
    struct gem_buffer *gem_buf = drm_f->gem;

    if (gem_buf->planes_count > cnt || !fd) {
        return -1;
    }
    for (i = 0; i < gem_buf->planes_count; i++)
        fd[i] = gem_buf->export_fds[i];
    return 0;
}

void display_engine_stop(void *handle)
{
  struct video_disp* disp = handle;

  disp->started = false;
  pthread_join (disp->disp_t, NULL);
  close_buffer (disp->drm_cli_fd, &disp->osd_gem_buf);
  if (disp->avsync)
    av_sync_destroy (disp->avsync);

#if 0
  if (disp->queue)
    free(disp->queue);
#endif
  if (gem_buf)
    free (gem_buf);
  if (disp->setup.plane) {
    if (disp->setup.plane->props_info)
      free (disp->setup.plane->props_info);
    free (disp->setup.plane);
  }
  if (disp->setup.crtc) {
    if (disp->setup.crtc->props_info)
      free (disp->setup.crtc->props_info);
    free (disp->setup.crtc);
  }
  if (disp->setup.connector) {
    if (disp->setup.connector->props_info)
      free (disp->setup.connector->props_info);
    free (disp->setup.connector);
  }
  if (disp->drm_fd >= 0)
    drmClose (disp->drm_fd);
  if (disp->drm_cli_fd >= 0)
    drmClose (disp->drm_cli_fd);

  free (disp);
}

static void * display_thread_func(void * arg)
{
  struct video_disp *disp = arg;
  struct vframe *sync_frame;
  struct drm_frame *f = NULL, *f_p1 = NULL, *f_p2 = NULL, *f_p3 = NULL;
  drmVBlank vbl;

  memset(&vbl, 0, sizeof(drmVBlank));

  while (disp->started) {
    int rc;
    struct gem_buffer* gem_buf;

    vbl.request.type = DRM_VBLANK_RELATIVE;
    vbl.request.sequence = 1;
    vbl.request.signal = 0;

    rc = drmWaitVBlank(disp->drm_fd, &vbl);
    if (rc) {
      GST_ERROR ("drmWaitVBlank error %d\n", rc);
      return NULL;
    }

    sync_frame = av_sync_pop_frame(disp->avsync);
    if (!sync_frame)
      continue;

    f = sync_frame->private;

    if (!f) {
      disp->last_frame = true;
      break;
    }

    GST_DEBUG ("pop frame: %u", f->pts);
    if (f != f_p1) {
      gem_buf = f->gem;
      rc = page_flip(disp, gem_buf);
      if (rc) {
        GST_ERROR ("page_flip error\n");
        continue;
      }
      /* 2 fence delay on video layer, 1 fence delay on osd */
      if (f_p3) {
        f_p3->displayed = true;
        display_cb(disp->priv, f_p3->pri_dec);
      }

      f_p3 = f_p2;
      f_p2 = f_p1;
      f_p1 = f;
    }
  }
  GST_INFO ("quit %s\n", __func__);
  return NULL;
}

static void sync_frame_free(struct vframe * sync_frame)
{
  struct drm_frame* drm_f = sync_frame->private;
  struct video_disp *disp = drm_f->pri_drm;

  if (drm_f) {
    drm_f->displayed = false;
    display_cb(disp->priv, drm_f->pri_dec);
    free(sync_frame);
  } else
    disp->last_frame = true;
}

int display_engine_show(void* handle, struct drm_frame* frame, struct rect* window)
{
  struct video_disp *disp = handle;
  int rc;
  struct vframe* sync_frame = &frame->sync_frame;

  if (!disp->started) {
    disp->started = true;
    disp->last_frame = false;

    rc = pthread_create(&disp->disp_t, NULL, display_thread_func, disp);
    if (rc) {
      GST_ERROR ("create dispay thread fails\n");
      return -1;
    }
  }

  if (!frame->last_flag) {
    sync_frame->private = frame;
    sync_frame->pts = frame->pts;
    sync_frame->duration = frame->duration;
    //TODO: set windows
  }
  sync_frame->free = sync_frame_free;
  while (disp->started) {
    if (av_sync_push_frame(disp->avsync, sync_frame)) {
      usleep(1000);
      continue;
    } else
      break;
  }
  GST_DEBUG ("push frame: %u", sync_frame->pts);

  return 0;
}

int display_engine_register_cb(displayed_cb_func cb)
{
  display_cb = cb;
  return 0;
}

int display_flush (void *handle)
{
  struct video_disp *disp = handle;
  while (disp->started && !disp->last_frame) {
    usleep(10);
  }
  if (!disp->started)
    return -1;

  return 0;
}

int display_set_pause(void *handle, bool pause)
{
  struct video_disp *disp = handle;

  return av_sync_pause (disp->avsync, pause);
}
