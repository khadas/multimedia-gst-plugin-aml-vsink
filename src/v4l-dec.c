/*
 * Copyright (c) 2020 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <gst/gstinfo.h>

#include "v4l-dec.h"
#include "aml_driver.h"

GST_DEBUG_CATEGORY_EXTERN(gst_aml_vsink_debug);
#define GST_CAT_DEFAULT gst_aml_vsink_debug

#define MIN_OUTPUT_BUFFERS (1)
#define MIN_CAPTURE_BUFFERS (3)
#define OUTPUT_BUFFER_SIZE (0x400000)
#define OUTPUT_BUFFER_SIZE_2K (0xC0000)
#define EXTRA_CAPTURE_BUFFERS (4)
static const char* video_dev_name = "/dev/video26";

int v4l_dec_open(bool sanity_check)
{
  int fd, rc;
  uint32_t deviceCaps;
  struct v4l2_capability caps;
  struct v4l2_exportbuffer eb;

  fd = open (video_dev_name, O_RDWR | O_CLOEXEC);
  if ( fd < 0 ) {
    GST_ERROR ("can not open %s errno %d", video_dev_name, errno);
    goto error;
  }

  if (!sanity_check)
    return fd;

  rc = ioctl (fd, VIDIOC_QUERYCAP, &caps);
  if (rc) {
    GST_ERROR ("VIDIOC_QUERYCAP fails errno:%d", errno);
    goto error;
  }

  /* driver sanity check */
  deviceCaps = (caps.capabilities & V4L2_CAP_DEVICE_CAPS ) ?
    caps.device_caps : caps.capabilities;

  if (!(deviceCaps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE))) {
    GST_ERROR ("V4L2_CAP_VIDEO_M2M_MPLANE not supported");
    goto error;
  }

  if (!(deviceCaps & V4L2_CAP_STREAMING) ) {
    GST_ERROR ("V4L2_CAP_STREAMING not supported");
    goto error;
  }

  eb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  eb.index = -1;
  eb.plane = -1;
  eb.flags = (O_RDWR|O_CLOEXEC);
  ioctl (fd, VIDIOC_EXPBUF, &eb);
  if (errno == ENOTTY) {
    GST_ERROR ("dmabuf not supported");
    goto error;
  }

  return fd;
error:
  if (fd > 0 )
    close( fd );

  return -1;
}

struct v4l2_fmtdesc* v4l_get_output_port_formats (int fd, uint32_t *num)
{
  struct v4l2_fmtdesc *formats = NULL;
  struct v4l2_fmtdesc fmtdesc;
  uint32_t fnum;
  int i = 0, rc;

  for ( ; ; )
  {
    fmtdesc.index= i;
    fmtdesc.type= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    rc = ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc);
    if (rc) {
      if (errno == EINVAL) {
        GST_DEBUG("Found %d output formats", i);
        fnum = i;
        break;
      }
      GST_ERROR ("VIDIOC_ENUM_FMT fail errno: %d\n", errno);
      goto error;
    }
    ++i;
  }

  formats = (struct v4l2_fmtdesc*)calloc (fnum, sizeof(*formats));
  if (!formats) {
    GST_ERROR("oom");
    goto error;
  }

  for (i= 0; i < fnum; ++i) {
    formats[i].index= i;
    formats[i].type= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    rc = ioctl (fd, VIDIOC_ENUM_FMT, &formats[i]);
    if (rc) {
      GST_ERROR("VIDIOC_ENUM_FMT index %d fail", i);
      goto error;
    }
    GST_DEBUG("output %d: flags %08x pixelFormat: %x desc: %s",
        i, formats[i].flags, formats[i].pixelformat, formats[i].description );
  }

  *num = fnum;
  return formats;

error:
  if (formats)
    free (formats);
  return NULL;
}

struct v4l2_fmtdesc* v4l_get_capture_port_formats(int fd, uint32_t *num)
{
   struct v4l2_fmtdesc format;
   int i, rc;
   uint32_t fnum;
   struct v4l2_fmtdesc *formats = NULL;

   for (i = 0; ; ++i) {
      format.index = i;
      format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      rc= ioctl(fd, VIDIOC_ENUM_FMT, &format);
      if ( rc < 0 )
      {
         if ( errno == EINVAL )
         {
            GST_DEBUG("Found %d capture formats", i);
            fnum = i;
            break;
         }
         goto exit;
      }
   }

   formats = (struct v4l2_fmtdesc*)calloc (fnum, sizeof(*formats));
   if (!formats) {
      GST_ERROR("oom");
      goto exit;
   }

   for (i = 0; i < fnum; ++i) {
      formats[i].index = i;
      formats[i].type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      rc= ioctl(fd, VIDIOC_ENUM_FMT, &formats[i]);
      if (rc) {
        GST_ERROR("VIDIOC_ENUM_FMT index %d fail errno %d", i, errno);
        goto exit;
      }
      GST_DEBUG("capture format %d: flags %08x pixelFormat: %x desc: %s",
             i, formats[i].flags, formats[i].pixelformat, formats[i].description );
   }

   return formats;

exit:
  if (formats)
    g_free (formats);
  return NULL;
}

int v4l_reg_event(int fd)
{
  int rc;
  struct v4l2_event_subscription evtsub;

  memset(&evtsub, 0, sizeof(evtsub));
  evtsub.type = V4L2_EVENT_SOURCE_CHANGE;

  rc = ioctl (fd, VIDIOC_SUBSCRIBE_EVENT, &evtsub );
  if (rc)
    GST_ERROR("event subscribe failed rc %d (errno %d)", rc, errno);

  memset(&evtsub, 0, sizeof(evtsub));
  evtsub.type = V4L2_EVENT_EOS;

  rc = ioctl (fd, VIDIOC_SUBSCRIBE_EVENT, &evtsub );
  if (rc)
    GST_ERROR("event subscribe failed rc %d (errno %d)", rc, errno);

  return rc;
}

int v4l_unreg_event(int fd)
{
  int rc;
  struct v4l2_event_subscription evtsub;

  memset(&evtsub, 0, sizeof(evtsub));

  evtsub.type= V4L2_EVENT_ALL;
  rc = ioctl(fd, VIDIOC_UNSUBSCRIBE_EVENT, &evtsub );
  if (rc)
    GST_ERROR("wstStopEvents: event unsubscribe failed rc %d (errno %d)", rc, errno);
  return rc;
}

struct output_buffer** v4l_setup_output_port (int fd, uint32_t mode, uint32_t *buf_cnt)
{
	int rc, cnt = 0;
	struct v4l2_control ctl;
	struct v4l2_requestbuffers reqbuf;
	struct v4l2_buffer *buf;
	int i;
  struct output_buffer **ob = NULL;

	memset(&ctl, 0, sizeof(ctl));
	ctl.id = V4L2_CID_MIN_BUFFERS_FOR_OUTPUT;
	rc = ioctl(fd, VIDIOC_G_CTRL, &ctl );
	if (!rc && ctl.value != 0) {
		cnt = ctl.value;
	} else {
    GST_ERROR ("fail get min buffer number %d", errno);
    goto error;
  }

	memset( &reqbuf, 0, sizeof(reqbuf) );
	reqbuf.count = cnt;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	reqbuf.memory = mode;
	rc = ioctl (fd, VIDIOC_REQBUFS, &reqbuf);
	if (rc) {
		GST_ERROR("failed to request %d output buffers rc %d errno %d", cnt, rc, errno);
		goto error;
	}

	if (reqbuf.count < MIN_OUTPUT_BUFFERS) {
		GST_ERROR("insufficient buffers: (%d vs %d)", reqbuf.count, MIN_OUTPUT_BUFFERS);
		goto error;
	}

  cnt = reqbuf.count;
  GST_DEBUG ("output port requires %d buffers", cnt);
  ob = (struct output_buffer **) calloc (cnt, sizeof(struct output_buffer *));
  if (!ob) {
    GST_ERROR ("oom");
    goto error;
  }
  for (i = 0 ; i < cnt ; i++) {
    ob[i] = (struct output_buffer *)calloc(1, sizeof(struct output_buffer));
    if (!ob[i]) {
      GST_ERROR("oom" );
      goto error;
    }
  }

	for (i = 0; i < cnt; ++i) {
    ob[i]->id = i;
		buf = &ob[i]->buf;
		buf->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		buf->index = i;
		buf->memory = mode;
    buf->m.planes = &ob[i]->plane;
    buf->length = 1;

		rc = ioctl (fd, VIDIOC_QUERYBUF, buf);
		if (rc) {
			GST_ERROR("failed to query input buffer %d: rc %d errno %d", i, rc, errno);
			goto error;
		}
		if (mode == V4L2_MEMORY_MMAP) {
      uint32_t memOffset, memLength, memBytesUsed;
      void *bufStart;

      if (buf->length != 1) {
        GST_ERROR("num planes expected to be 1 vs %d", buf->length);
        goto error;
      }

      memOffset= buf->m.planes[0].m.mem_offset;
      memLength= buf->m.planes[0].length;
      memBytesUsed= buf->m.planes[0].bytesused;

			bufStart = mmap(NULL, memLength, PROT_READ | PROT_WRITE,
					MAP_SHARED, fd, memOffset);
			if (bufStart == MAP_FAILED) {
				GST_ERROR("failed to mmap input buffer %d: errno %d", i, errno);
				goto error;
			}

			GST_LOG ("index: %d start: %p bytesUsed %d  offset %d length %d flags %08x",
					buf->index, bufStart, memBytesUsed, memOffset, memLength, buf->flags);

			ob[i]->vaddr = bufStart;
			ob[i]->size = memLength;
		} else if (mode == V4L2_MEMORY_DMABUF)
				ob[i]->plane.m.fd= -1;
	}

  *buf_cnt = cnt;
  GST_LOG ("alloc %d buffers from %p", cnt, ob);
  return ob;

error:
  recycle_output_port_buffer (fd, ob, cnt);
	return NULL;
}

void recycle_output_port_buffer (int fd, struct output_buffer **ob, uint32_t num)
{
  if (ob) {
    int i, ret;
    struct v4l2_requestbuffers req = {
      .memory = V4L2_MEMORY_DMABUF,
      .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      .count = 0,
    };

    GST_LOG ("recycle %d buffers from %p", num, ob);
    if (!num)
      return;

    ret = ioctl(fd, VIDIOC_REQBUFS, &req);
    if (ret) {
      GST_ERROR ("fail VIDIOC_REQBUFS %d",errno);
      return;
    }

    for (i = 0 ; i < num ; i++) {
      if (!ob[i])
        continue;
      if (ob[i]->vaddr)
        munmap (ob[i]->vaddr, ob[i]->size);
      if (ob[i]->gstbuf) {
        gst_buffer_unref (ob[i]->gstbuf);
        ob[i]->gstbuf = NULL;
      }
      free (ob[i]);
    }
    free (ob);
  }
}

struct capture_buffer** v4l_setup_capture_port (int fd, uint32_t *buf_cnt,
    uint32_t dw_mode, void *drm_handle, uint32_t *coded_w, uint32_t *coded_h,
    bool secure, bool pip, bool is_2k_only)
{
  int rc, i, j;
  struct v4l2_format fmt;
  uint32_t pixelFormat;
  struct capture_buffer **cb = NULL;
  int cnt = 0;
  struct v4l2_control ctl;
  struct v4l2_requestbuffers reqbuf;
  uint32_t w,h;


  memset (&fmt, 0, sizeof(struct v4l2_format));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  rc = ioctl(fd, VIDIOC_G_FMT, &fmt);
  if (rc) {
    GST_ERROR ("VIDIOC_G_FMT cap error %d", errno);
    return NULL;
  }

  w = fmt.fmt.pix_mp.width;
  h = fmt.fmt.pix_mp.height;
  GST_WARNING ("buffer size %dx%d", w, h);

  if (dw_mode != VDEC_DW_AFBC_ONLY) {
    fmt.fmt.pix_mp.num_planes = 2;
    pixelFormat = V4L2_PIX_FMT_NV12M;
  } else {
    //single plane for AFBC only buffer
    fmt.fmt.pix_mp.num_planes = 1;
    pixelFormat = V4L2_PIX_FMT_NV12;
  }

  fmt.fmt.pix_mp.pixelformat = pixelFormat;
  rc = ioctl (fd, VIDIOC_S_FMT, &fmt);
  if (rc) {
    GST_DEBUG ("failed to set format for capture: rc %d errno %d", rc, errno);
    goto exit;
  }

  /* minimium capture buffer number */
  memset( &ctl, 0, sizeof(ctl));
  ctl.id= V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
  rc = ioctl (fd, VIDIOC_G_CTRL, &ctl);
  if (!rc)
    cnt = ctl.value;
  if (!cnt)
    cnt = MIN_OUTPUT_BUFFERS;
  GST_DEBUG ("min capture buffer number %d", cnt);

  /* REQBUFS */
  memset (&reqbuf, 0, sizeof(reqbuf));
  reqbuf.count = cnt;
  reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  reqbuf.memory = V4L2_MEMORY_DMABUF;
  rc = ioctl (fd, VIDIOC_REQBUFS, &reqbuf );
  if (rc) {
    GST_ERROR ("failed to request %d capture buffers rc %d errno %d", cnt, rc, errno);
    goto exit;
  }

  if (reqbuf.count < cnt) {
    GST_ERROR ("insufficient buffers: (%d versus %d)", reqbuf.count, cnt);
    goto exit;
  }

  GST_DEBUG ("capture port requires %d buffers", reqbuf.count);
  cb = (struct capture_buffer **)calloc (reqbuf.count, sizeof(struct capture_buffer *));
  if (!cb) {
    GST_ERROR ("oom");
    goto exit;
  }

  for (i = 0 ; i < reqbuf.count ; ++i) {
    struct v4l2_buffer *buf;
    int fds[4] = {0};

    cb[i] = (struct capture_buffer *) calloc (1, sizeof (struct capture_buffer));
    if (!cb[i]) {
      GST_ERROR ("oom %d", i);
      goto exit;
    }
    cb[i]->id = i;
    cb[i]->drm_frame = display_create_buffer (drm_handle,
        w, h,
        (dw_mode == VDEC_DW_AFBC_ONLY)? FRAME_FMT_AFBC:FRAME_FMT_NV12,
        fmt.fmt.pix_mp.num_planes,
        secure, pip);

    if (!cb[i]->drm_frame) {
      GST_ERROR ("drm fail to alloc gem buffer %dx%d %d", w, h, i);
      goto exit;
    }

    buf = &cb[i]->buf;
    buf->index = i;
    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf->memory = V4L2_MEMORY_DMABUF;
    buf->length = 2;
    buf->m.planes = cb[i]->plane;

    rc = ioctl (fd, VIDIOC_QUERYBUF, buf);
    if (rc) {
      GST_ERROR ("VIDIOC_QUERYBUF %d cap buf fail errno %d", i, errno);
      goto exit;
    }
    rc = display_get_buffer_fds(cb[i]->drm_frame,
        fds, fmt.fmt.pix_mp.num_planes);
    if (rc) {
      GST_ERROR("get fd error");
      goto exit;
    }
    for (j = 0 ; j < fmt.fmt.pix_mp.num_planes; ++j) {
      cb[i]->vaddr[j] = NULL;
      cb[i]->plane[j].m.fd = fds[j];
      cb[i]->gem_fd[j] = fds[j];
    }

    rc = ioctl (fd, VIDIOC_QBUF, buf);
    if (rc) {
      GST_ERROR ("VIDIOC_QBUF cap %d error", i);
      goto exit;
    }
    GST_LOG ("queue cb %d", i);
  }

  *buf_cnt = cnt;
  *coded_w = w;
  *coded_h = h;
  return cb;

exit:
  recycle_capture_port_buffer (fd, cb, cnt);
  return NULL;
}

int recycle_capture_port_buffer (int fd, struct capture_buffer **cb, uint32_t num)
{
  int i, ret, rel_num = 0;

  if (cb) {
    struct v4l2_requestbuffers req = {
      .memory = V4L2_MEMORY_DMABUF,
      .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
      .count = 0,
    };

    ret = ioctl(fd, VIDIOC_REQBUFS, &req);
    if (ret) {
      GST_ERROR ("fail VIDIOC_REQBUFS %d",errno);
      return;
    }

    for (i = 0 ; i < num ; i++) {
      if (!cb[i]) {
        GST_WARNING ("index %d freed", i);
        continue;
      }
      if (!cb[i]->displayed) {
        if (cb[i]->drm_frame) {
          cb[i]->drm_frame->destroy(cb[i]->drm_frame);
          rel_num++;
          GST_DEBUG ("free index %d", i);
        }
        free (cb[i]);
        cb [i] = NULL;
      } else {
        cb[i]->free_on_recycle = true;
      }
    }
    free (cb);
  }
  return rel_num;
}

int v4l_dec_dw_config(int fd, uint32_t fmt, uint32_t dw_mode, bool only_2k)
{
  int rc;
  struct v4l2_streamparm streamparm;
  struct aml_dec_params *decParm = (struct aml_dec_params*)streamparm.parm.raw_data;

  memset (&streamparm, 0, sizeof(streamparm));
  streamparm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  decParm->parms_status = V4L2_CONFIG_PARM_DECODE_CFGINFO;
  decParm->cfg.double_write_mode = dw_mode;

  if (fmt == V4L2_PIX_FMT_H264)
    decParm->cfg.ref_buf_margin = EXTRA_CAPTURE_BUFFERS + 1;
  else if (fmt == V4L2_PIX_FMT_AV1 && only_2k)
    decParm->cfg.ref_buf_margin = EXTRA_CAPTURE_BUFFERS - 1;
  else if (fmt != V4L2_PIX_FMT_MPEG2 && fmt != V4L2_PIX_FMT_MPEG1)
    decParm->cfg.ref_buf_margin = EXTRA_CAPTURE_BUFFERS;

  rc = ioctl (fd, VIDIOC_S_PARM, &streamparm );
  if (rc)
    GST_ERROR("VIDIOC_S_PARAM failed for aml driver raw_data: %d", rc);

  return rc;
}

int v4l_dec_config(int fd, bool secure, uint32_t fmt, uint32_t dw_mode,
    struct hdr_meta *hdr, bool is_2k_only)
{
  int rc;
  struct v4l2_streamparm streamparm;
  struct aml_dec_params *decParm = (struct aml_dec_params*)streamparm.parm.raw_data;
  struct v4l2_format v4l_fmt;
  int32_t w, h;

  memset(&v4l_fmt, 0, sizeof(struct v4l2_format));
  v4l_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  rc = ioctl(fd, VIDIOC_G_FMT, &v4l_fmt);
  if (rc) {
    GST_ERROR ("VIDIOC_G_FMT cap error %d", errno);
    return rc;
  }

  w = v4l_fmt.fmt.pix_mp.width;
  h = v4l_fmt.fmt.pix_mp.height;
  GST_INFO ("coded size %dx%d", w, h);

  if (is_2k_only) {
    if (w > 1920 || h > 1088) {
      GST_ERROR ("don't support %dx%d > 2k", w, h);
      return -1;
    }
  }

  memset (&streamparm, 0, sizeof(streamparm));
  streamparm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  decParm->parms_status = V4L2_CONFIG_PARM_DECODE_CFGINFO;

  decParm->cfg.double_write_mode = dw_mode;
  if (fmt != V4L2_PIX_FMT_MPEG2)
    decParm->cfg.ref_buf_margin = EXTRA_CAPTURE_BUFFERS;
  decParm->cfg.metadata_config_flag |= (1 << 12);
  decParm->cfg.metadata_config_flag |= (1 << 13);


	if (hdr->haveColorimetry || hdr->haveMasteringDisplay ||
			hdr->haveContentLightLevel) {
		decParm->parms_status |= V4L2_CONFIG_PARM_DECODE_HDRINFO;
		if (hdr->haveColorimetry) {
			decParm->hdr.signal_type = (1<<29); /* present flag */
			/* range */
			switch (hdr->Colorimetry[0]) {
      case 1:
			case 2:
			  decParm->hdr.signal_type |= ((hdr->Colorimetry[0] % 2)<<25);
        break;
			default:
				break;
			}
			/* matrix coefficient */
			switch (hdr->Colorimetry[1]) {
      case 1: /* RGB */
        decParm->hdr.signal_type |= 0;
        break;
      case 2: /* FCC */
        decParm->hdr.signal_type |= 4;
        break;
      case 3: /* BT709 */
        decParm->hdr.signal_type |= 1;
        break;
      case 4: /* BT601 */
        decParm->hdr.signal_type |= 3;
        break;
      case 5: /* SMPTE240M */
        decParm->hdr.signal_type |= 7;
        break;
      case 6: /* BT2020 */
        decParm->hdr.signal_type |= 10;
        break;
      default: /* unknown */
        decParm->hdr.signal_type |= 2;
        break;
			}
			/* transfer function */
			switch (hdr->Colorimetry[2]) {
      case 5: /* BT709 */
          decParm->hdr.signal_type |= (1<<8);
        break;
      case 6: /* SMPTE240M */
        decParm->hdr.signal_type |= (7<<8);
        break;
      case 9: /* LOG100 */
        decParm->hdr.signal_type |= (9<<8);
        break;
      case 10: /* LOG316 */
        decParm->hdr.signal_type |= (10<<8);
        break;
      case 11: /* BT2020_12 */
        decParm->hdr.signal_type |= (15<<8);
        break;
      case 13: /* BT2020_10 */
        decParm->hdr.signal_type |= (14<<8);
        break;
      case 14: /* SMPTE2084 */
        decParm->hdr.signal_type |= (16<<8);
        break;
      case 16: /* BT601 */
        decParm->hdr.signal_type |= (3<<8);
        break;
      case 1: /* GAMMA10 */
      case 2: /* GAMMA18 */
      case 3: /* GAMMA20 */
      case 4: /* GAMMA22 */
      case 7: /* SRGB */
      case 8: /* GAMMA28 */
      case 12: /* ADOBERGB */
      case 15: /* ARIB_STD_B76 */
      default:
        break;
      }
      /* primaries */
      switch (hdr->Colorimetry[3]) {
      case 1: /* BT709 */
        decParm->hdr.signal_type |= ((1<<24)|(1<<16));
        break;
      case 2: /* BT470M */
        decParm->hdr.signal_type |= ((1<<24)|(4<<16));
        break;
      case 3: /* BT470BG */
        decParm->hdr.signal_type |= ((1<<24)|(5<<16));
        break;
      case 4: /* SMPTE170M */
        decParm->hdr.signal_type |= ((1<<24)|(6<<16));
        break;
      case 5: /* SMPTE240M */
        decParm->hdr.signal_type |= ((1<<24)|(7<<16));
        break;
      case 6: /* FILM */
        decParm->hdr.signal_type |= ((1<<24)|(8<<16));
        break;
      case 7: /* BT2020 */
        decParm->hdr.signal_type |= ((1<<24)|(9<<16));
        break;
      case 8: /* ADOBERGB */
      default:
        break;
      }
    }

    if (hdr->haveMasteringDisplay) {
      decParm->hdr.color_parms.present_flag= 1;
      decParm->hdr.color_parms.primaries[2][0]= (uint32_t)(hdr->MasteringDisplay[0]*50000); /* R.x */
      decParm->hdr.color_parms.primaries[2][1]= (uint32_t)(hdr->MasteringDisplay[1]*50000); /* R.y */
      decParm->hdr.color_parms.primaries[0][0]= (uint32_t)(hdr->MasteringDisplay[2]*50000); /* G.x */
      decParm->hdr.color_parms.primaries[0][1]= (uint32_t)(hdr->MasteringDisplay[3]*50000); /* G.y */
      decParm->hdr.color_parms.primaries[1][0]= (uint32_t)(hdr->MasteringDisplay[4]*50000); /* B.x */
      decParm->hdr.color_parms.primaries[1][1]= (uint32_t)(hdr->MasteringDisplay[5]*50000); /* B.y */
      decParm->hdr.color_parms.white_point[0]= (uint32_t)(hdr->MasteringDisplay[6]*50000);
      decParm->hdr.color_parms.white_point[1]= (uint32_t)(hdr->MasteringDisplay[7]*50000);
      decParm->hdr.color_parms.luminance[0]= (uint32_t)(hdr->MasteringDisplay[8]);
      decParm->hdr.color_parms.luminance[1]= (uint32_t)(hdr->MasteringDisplay[9]);
    }

    if (hdr->haveContentLightLevel) {
        decParm->hdr.color_parms.content_light_level.max_content= hdr->ContentLightLevel[0];
        decParm->hdr.color_parms.content_light_level.max_pic_average= hdr->ContentLightLevel[1];
    }
	}

  rc = ioctl (fd, VIDIOC_S_PARM, &streamparm );
  if (rc)
    GST_ERROR("VIDIOC_S_PARAM failed for aml driver raw_data: %d", rc);

  return rc;
}

int v4l_set_output_format(int fd, uint32_t format, int w, int h, bool only_2k)
{
  int rc;
  struct v4l2_format fmt;

  memset (&fmt, 0, sizeof(struct v4l2_format));
  fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  fmt.fmt.pix_mp.pixelformat = format;
  if (w > 0 && h > 0) {
    fmt.fmt.pix_mp.width = w;
    fmt.fmt.pix_mp.height = h;
  }
  fmt.fmt.pix_mp.num_planes= 1;
  if (only_2k)
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = OUTPUT_BUFFER_SIZE_2K;
  else
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = OUTPUT_BUFFER_SIZE;
  fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 0;
  fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
  rc = ioctl (fd, VIDIOC_S_FMT, &fmt);
  if (rc)
    GST_ERROR ("failed to set format for output: rc %d errno %d", rc, errno);

  return rc;
}

#if 0
static int config_sys_node(const char* path, const char* value)
{
  int fd;

  fd = open(path, O_RDWR);
  if (fd < 0) {
    GST_ERROR("fail to open %s\n", path);
    return -1;
  }
  if (write(fd, value, strlen(value)) != strlen(value)) {
    GST_ERROR("fail to write %s to %s\n", value, path);
    close(fd);
    return -1;
  }
  close(fd);
  g_print("set sysnode %s to %s\n", path, value);

  return 0;
}
#endif

int v4l_set_secure_mode(int fd, int w, int h, bool secure)
{
  int rc;
  struct v4l2_queryctrl queryctrl;
  struct v4l2_control control;

  memset( &queryctrl, 0, sizeof (queryctrl) );
  queryctrl.id= AML_V4L2_SET_DRMMODE;

  rc = ioctl (fd, VIDIOC_QUERYCTRL, &queryctrl );
  if (rc) {
    GST_ERROR("VIDIOC_QUERYCTRL fails");
    return rc;
  }
  if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) {
    GST_ERROR("AML_V4L2_SET_DRMMODE is disabled");
    return -1;
  }

  /* drm will handle this node, it has kernel refcnt,
   * don't touch it here
   */
#if 0
#define CODEC_MM_TVP "/sys/class/codec_mm/tvp_enable"
  if (secure) {
    GST_WARNING ("secure video");
    if ( w > 1920 || h > 1080)
      config_sys_node(CODEC_MM_TVP, "2");
    else if (w == -1 || h == -1)
      config_sys_node(CODEC_MM_TVP, "2");
    else
      config_sys_node(CODEC_MM_TVP, "1");
  } else {
    GST_WARNING ("non-secure video");
    config_sys_node(CODEC_MM_TVP, "0");
  }
#endif

  memset (&control, 0, sizeof (control) );
  control.id = AML_V4L2_SET_DRMMODE;
  control.value = (secure ? 1 : 0);
  rc = ioctl (fd, VIDIOC_S_CTRL, &control);
  if (rc)
    GST_ERROR("AML_V4L2_SET_DRMMODE fail: rc %d", rc);

  return rc;
}

int v4l_queue_capture_buffer(int fd, struct capture_buffer *cb)
{
  int ret;

  cb->displayed = false;
  ret = ioctl(fd, VIDIOC_QBUF, &cb->buf);
  if (ret) {
    GST_ERROR ("cap VIDIOC_QBUF %dth buf fail %d\n",
        cb->id, errno);
  }
  return ret;
}

