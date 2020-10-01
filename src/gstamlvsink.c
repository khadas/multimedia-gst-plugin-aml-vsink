/* GStreamer
 * Copyright (C) 2020 <song.zhao@amlogic.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstamlvsink
 *
 * The amlvsink element do video layer rendering directly
 *
 */

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <aml_avsync.h>
#include <gst/allocators/gstdmabuf.h>
//#include <gst/gstclock.h>
#include "gstamlvsink.h"
#include "v4l-dec.h"
#include "display.h"

GST_DEBUG_CATEGORY (gst_aml_vsink_debug);
#define GST_CAT_DEFAULT gst_aml_vsink_debug

#define PTS_90K 90000

struct _GstAmlVsinkPrivate
{
  gboolean paused;
  gboolean flushing_;
  gboolean received_eos;
  gboolean eos;
  guint32 seqnum; /* for eos */

  /* for position */
  gint64 position;
  GstClockTime first_ts;
  gboolean first_ts_set;

  GstSegment segment;
  /* curent stream group */
  guint group_id;
  gboolean group_done;

  /* scaling */
  gboolean scale_set;
  struct rect window;

  gboolean pip;

  /* v4l2 decoder */
  int fd;
  uint32_t dw_mode;
  bool secure;
  gboolean dw_mode_user_set;
  uint32_t output_format;
  uint32_t output_mode;
  struct v4l2_fmtdesc *output_formats;
  uint32_t ob_num;
  struct output_buffer **ob;
  gboolean output_start;
  gboolean output_port_config;

  uint32_t capture_format;
  struct v4l2_fmtdesc *capture_formats;
  uint32_t cb_num;
  struct capture_buffer **cb;

  /* output thread */
  gboolean quitVideoOutputThread;
  GThread *videoOutputThread;

  /* render */
  void *render;

  GstCaps *caps;

  /* ES info from caps */
  double fr;
  int es_width;
  int es_height;

  /* frame dimension */
  int visible_w;
  int visible_h;
  uint32_t coded_w;
  uint32_t coded_h;

  /* HDR */
  struct hdr_meta hdr;

  /* statistics */
  int in_frame_cnt;
  int out_frame_cnt;
  int dropped_frame_num;

  /* lock */
  pthread_mutex_t res_lock;
};

enum
{
  PROP_0,
  PROP_WINDOW_SET,
  PROP_PIP_VIDEO,
  PROP_VIDEO_FRAME_DROP_NUM,
  PROP_VIDEO_DW_MODE,
  PROP_LAST
};

#define gst_aml_vsink_parent_class parent_class
#if GST_CHECK_VERSION(1,14,0)
G_DEFINE_TYPE_WITH_CODE (GstAmlVsink, gst_aml_vsink, GST_TYPE_BASE_SINK,
  GST_DEBUG_CATEGORY_INIT (gst_aml_vsink_debug, "amlvsink", 0,
  "debug category for amlvsink element");G_ADD_PRIVATE(GstAmlVsink));
#else
#define GST_AML_VSINK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_AML_VSINK, GstAmlVsinkPrivate))

/* class initialization */
G_DEFINE_TYPE_WITH_CODE (GstAmlVsink, gst_aml_vsink, GST_TYPE_BASE_SINK,
  GST_DEBUG_CATEGORY_INIT (gst_aml_vsink_debug, "amlvsink", 0,
  "debug category for amlvsink element"));
#endif

enum
{
  SIGNAL_FIRSTFRAME,
  MAX_SIGNAL
};
static guint g_signals[MAX_SIGNAL]= {0};

static void gst_aml_vsink_dispose(GObject * object);

static void gst_aml_vsink_set_property(GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_aml_vsink_get_property(GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstStateChangeReturn gst_aml_vsink_change_state(GstElement *
    element, GstStateChange transition);
static gboolean gst_aml_vsink_query(GstElement * element, GstQuery *
    query);

static inline void vsink_reset (GstAmlVsink * sink);

static GstFlowReturn gst_aml_vsink_render (GstAmlVsink * sink, GstBuffer * buffer);
static GstFlowReturn gst_aml_vsink_chain (GstPad * pad, GstObject * parent, GstBuffer * buf);
static gboolean gst_aml_vsink_event(GstAmlVsink *sink, GstEvent * event);
static gboolean gst_aml_vsink_pad_event (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean gst_aml_vsink_setcaps (GstBaseSink * bsink, GstCaps * caps);

static void reset_decoder(GstAmlVsink *sink);
static gboolean check_vdec(GstAmlVsinkClass *klass);
int capture_buffer_recycle(void* priv_data, void* handle);
//static int get_sysfs_uint32(const char *path, uint32_t *value);
//static int config_sys_node(const char* path, const char* value);
#define DUMP_TO_FILE
#ifdef DUMP_TO_FILE
static guint file_index;
static void dump(const char* path, const uint8_t *data, int size, gboolean vp9, int frame_cnt);
#endif

static void
gst_aml_vsink_class_init (GstAmlVsinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  check_vdec (klass);

#if GST_CHECK_VERSION(1,14,0)
#else
  g_type_class_add_private (klass, sizeof (GstAmlVsinkPrivate));
#endif

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "Amlogic Video sink", "Codec/Decoder/Video/Sink/Video",
      "Display video frame on video plane",
      "song.zhao@amlogic.com");

  gobject_class->set_property = gst_aml_vsink_set_property;
  gobject_class->get_property = gst_aml_vsink_get_property;
  gobject_class->dispose = gst_aml_vsink_dispose;

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_WINDOW_SET,
      g_param_spec_string ("rectangle", "rectangle",
        "Window Set Format: x,y,width,height",
        NULL, G_PARAM_WRITABLE));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_VIDEO_FRAME_DROP_NUM,
      g_param_spec_int ("frames-dropped", "frames-dropped",
        "number of dropped frames",
        0, G_MAXINT32, 0, G_PARAM_READABLE));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_PIP_VIDEO,
      g_param_spec_boolean ("pip", "pip",
        "video picture in picture",
        FALSE, G_PARAM_WRITABLE));

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_VIDEO_DW_MODE,
      g_param_spec_int ("double-write-mode", "double-write-mode",
        "0/1/2/4/16 Only 16 is valid for h264/mepg2.",
        0, 16, 0, G_PARAM_WRITABLE));

  g_signals[SIGNAL_FIRSTFRAME]= g_signal_new( "first-video-frame-callback",
      G_TYPE_FROM_CLASS(GST_ELEMENT_CLASS(klass)),
      (GSignalFlags) (G_SIGNAL_RUN_LAST),
      0,    /* class offset */
      NULL, /* accumulator */
      NULL, /* accu data */
      g_cclosure_marshal_VOID__UINT_POINTER,
      G_TYPE_NONE,
      2,
      G_TYPE_UINT,
      G_TYPE_POINTER );

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_aml_vsink_change_state);
  gstelement_class->query = GST_DEBUG_FUNCPTR (gst_aml_vsink_query);

  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_aml_vsink_setcaps);
}

static gboolean build_caps(GstAmlVsinkClass*klass, struct v4l2_fmtdesc *formats, uint32_t fnum)
{
  gboolean ret = FALSE;
  GstCaps *caps = 0;
  GstCaps *tmp = 0;
  GstPadTemplate *padTemplate= 0;
  int i;

  caps= gst_caps_new_empty();
  if (!caps ) {
    GST_ERROR("gst_caps_new_empty failed");
    return FALSE;
  }

  for (i= 0; i < fnum; ++i) {
    switch (formats[i].pixelformat) {
    case V4L2_PIX_FMT_MPEG2:
      tmp = gst_caps_from_string(
          "video/mpeg, " \
          "mpegversion=(int) 2, " \
          "parsed=(boolean) true, " \
          "systemstream = (boolean) false ; "
          );
      break;
    case V4L2_PIX_FMT_MPEG4:
      tmp = gst_caps_from_string(
          "video/mpeg, " \
          "mpegversion=(int) 4, " \
          "parsed=(boolean) true, " \
          "systemstream = (boolean) false ; "
          );
      break;
    case V4L2_PIX_FMT_H264:
      tmp = gst_caps_from_string(
          "video/x-h264, " \
          "parsed=(boolean) true, " \
          "alignment=(string) au, " \
          "stream-format=(string) byte-stream, " \
          "width=(int) [1,MAX], " "height=(int) [1,MAX] ; " \
          "video/x-h264(memory:DMABuf) ; "
          );
      break;
    case V4L2_PIX_FMT_VP9:
      tmp = gst_caps_from_string(
          "video/x-vp9, " \
          "width=(int) [1,MAX], " \
          "height=(int) [1,MAX] ; " \
          "video/x-vp9(memory:DMABuf) ; "
          );
      break;
    case V4L2_PIX_FMT_HEVC:
      tmp = gst_caps_from_string(
          "video/x-h265, " \
          "parsed=(boolean) true, " \
          "alignment=(string) au, " \
          "stream-format=(string) byte-stream, " \
          "width=(int) [1,MAX], " \
          "height=(int) [1,MAX] ; " \
          "video/x-h265(memory:DMABuf) ; "
          );
      break;
    default:
      break;
    }
    if (tmp) {
      gst_caps_append( caps, tmp );
      tmp = NULL;
    }
  }

  padTemplate= gst_pad_template_new( "sink",
      GST_PAD_SINK,
      GST_PAD_ALWAYS,
      caps );
  if (padTemplate)
  {
    GstElementClass *element_class = (GstElementClass *)klass;
    gst_element_class_add_pad_template (element_class, padTemplate);
  } else {
    GST_ERROR("gst_pad_template_new failed");
    goto error;
  }

  ret = TRUE;
error:
  gst_caps_unref (caps);
  return ret;
}

static gboolean check_vdec(GstAmlVsinkClass *klass)
{
  gboolean ret = FALSE;
  int fd;
  uint32_t fnum;
  struct v4l2_fmtdesc *formats = NULL;

  GST_TRACE ("open vdec");
  fd = v4l_dec_open();
  if (fd < 0) {
    GST_ERROR("dec ope fail");
    goto error;
  }

  formats = v4l_get_output_port_formats (fd, &fnum);
  if (!formats) {
    GST_ERROR ("can not get formats");
    goto error;
  }

  if (!build_caps (klass, formats, fnum)) {
    GST_ERROR ("can not build caps");
    goto error;
  }

  GST_TRACE ("done");
  close (fd);
  ret = TRUE;
error:
  if (fd >= 0 )
    close( fd );

  if (formats)
    g_free (formats);

  return ret;
}

static void
gst_aml_vsink_init (GstAmlVsink* sink)
{
  GstBaseSink *basesink;
#if GST_CHECK_VERSION(1,14,0)
  GstAmlVsinkPrivate *priv = gst_aml_vsink_get_instance_private (sink);
#else
  GstAmlVsinkPrivate *priv = GST_AML_VSINK_GET_PRIVATE (sink);
#endif

  sink->priv = priv;
  basesink = GST_BASE_SINK_CAST (sink);
  /* bypass sync control of basesink */
  gst_base_sink_set_sync (basesink, FALSE);
  gst_pad_set_event_function (basesink->sinkpad, gst_aml_vsink_pad_event);
  gst_pad_set_chain_function (basesink->sinkpad, gst_aml_vsink_chain);

  pthread_mutex_init (&priv->res_lock, NULL);
  priv->received_eos = FALSE;
  priv->group_id = -1;
  priv->fd = -1;
}

static void
gst_aml_vsink_dispose (GObject * object)
{
  GstAmlVsink* sink = GST_AML_VSINK(object);
  GstAmlVsinkPrivate *priv = sink->priv;

  pthread_mutex_destroy (&priv->res_lock);
  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static gboolean
gst_aml_vsink_query (GstElement * element, GstQuery * query)
{
  gboolean res = FALSE;
  GstAmlVsink *sink = GST_AML_VSINK (element);
  GstAmlVsinkPrivate *priv = sink->priv;

  sink = GST_AML_VSINK (element);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
    {
      gst_query_set_latency (query, FALSE, 0, 10*1000*1000);
      return TRUE;
    }
    case GST_QUERY_POSITION:
    {
      GstFormat format;
      gst_query_parse_position (query, &format, NULL);

      if (GST_FORMAT_BYTES == format)
        return GST_ELEMENT_CLASS (parent_class)->query (element, query);

      GST_LOG_OBJECT(sink, "POSITION: %lld", priv->position);
      gst_query_set_position (query, GST_FORMAT_TIME, priv->position);
      break;
    }
    default:
      res = GST_ELEMENT_CLASS (parent_class)->query (element, query);
      break;
  }
  return res;
}



static void
gst_aml_vsink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAmlVsink *sink = GST_AML_VSINK (object);
  GstAmlVsinkPrivate *priv = sink->priv;

  switch (property_id) {
  case PROP_VIDEO_DW_MODE:
  {
    int mode = g_value_get_int (value);
    if (mode == VDEC_DW_AFBC_ONLY ||
        mode == VDEC_DW_AFBC_1_1_DW ||
        mode == VDEC_DW_AFBC_1_4_DW ||
        mode == VDEC_DW_AFBC_1_2_DW ||
        mode == VDEC_DW_NO_AFBC) {
      priv->dw_mode = mode;
      priv->dw_mode_user_set = TRUE;
      GST_WARNING_OBJECT (sink, "double write mode %d", priv->dw_mode);
    } else {
      GST_ERROR_OBJECT (sink, "invalid dw mode %d", priv->dw_mode);
    }
    break;
  }
  case PROP_PIP_VIDEO:
  {
    priv->pip = g_value_get_boolean (value);
    GST_WARNING_OBJECT(sink, "pip video enabled %d", priv->pip);
    break;
  }
  case PROP_WINDOW_SET:
  {
    const gchar *str = g_value_get_string (value);
    gchar **parts = g_strsplit (str, ",", 4);

    if ( !parts[0] || !parts[1] || !parts[2] || !parts[3] ) {
      GST_ERROR( "Bad window properties string" );
    } else {
      int nx, ny, nw, nh;

      nx = atoi(parts[0]);
      ny = atoi(parts[1]);
      nw = atoi(parts[2]);
      nh = atoi(parts[3]);

      if ( (!priv->scale_set) ||
          (nx != priv->window.x) ||
          (ny != priv->window.y) ||
          (nw != priv->window.w) ||
          (nh != priv->window.h) ) {
        //TODO: lock logic
        GST_OBJECT_LOCK ( sink );
        priv->scale_set = true;
        priv->window.x = nx;
        priv->window.y = ny;
        priv->window.w = nw;
        priv->window.h = nh;
        GST_OBJECT_UNLOCK ( sink );

        GST_WARNING ("set window rect (%d,%d,%d,%d)\n", nx, ny, nw, nh);
        //TODO: set scaling here
      }
    }
    g_strfreev(parts);
    break;
  }
  default:
  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  break;
  }
}

static void gst_aml_vsink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstAmlVsink *sink = GST_AML_VSINK (object);
  GstAmlVsinkPrivate *priv = sink->priv;

  switch (property_id) {
  case PROP_VIDEO_FRAME_DROP_NUM:
    g_value_set_int(value, priv->dropped_frame_num);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}

static gboolean gst_aml_vsink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstAmlVsink *sink = GST_AML_VSINK (bsink);
  GstAmlVsinkPrivate *priv = sink->priv;
  GstStructure *structure;
  const gchar *mime;
  int len;
  gint num, denom, width, height;

  if (G_UNLIKELY (priv->caps && gst_caps_is_equal (priv->caps, caps))) {
    GST_DEBUG_OBJECT (sink,
        "caps haven't changed, skipping reconfiguration");
    return TRUE;
  }

  gchar *str= gst_caps_to_string (caps);
  GST_INFO ("caps: %s", str);
  g_free(str);

  structure= gst_caps_get_structure (caps, 0);
  if (!structure)
    return FALSE;

  mime= gst_structure_get_name (structure);
  if (!mime)
    return FALSE;

  /* format */
  priv->output_format = -1;
  len= strlen(mime);
  if (len == 12 && !strncmp ("video/x-h264", mime, len))
    priv->output_format = V4L2_PIX_FMT_H264;
  else if (len == 10 && !strncmp ("video/mpeg", mime, len)) {
    int version;

    if (gst_structure_get_int (structure, "mpegversion", &version)
      && version == 2)
      priv->output_format = V4L2_PIX_FMT_MPEG2;
  } else if (len == 12 && !strncmp ("video/x-h265", mime, len))
    priv->output_format = V4L2_PIX_FMT_HEVC;
  else if (len == 11 && !strncmp ("video/x-vp9", mime, len))
    priv->output_format = V4L2_PIX_FMT_VP9;

  if (priv->output_format == -1) {
    GST_ERROR("not accepting format(%s)", mime );
    goto error;
  }

  /* frame rate */
	if (gst_structure_get_fraction (structure, "framerate", &num, &denom)) {
		if ( denom == 0 )
      denom= 1;

		priv->fr = (double)num/(double)denom;
		if (priv->fr <= 0.0) {
			g_print("assume 60 fps\n");
			priv->fr = 60.0;
		}
	}

  /* dimension */
	if (gst_structure_get_int (structure, "width", &width ))
    priv->es_width = width;
	if (gst_structure_get_int (structure, "height", &height))
    priv->es_height = width;

  /* setup double write mode */
  switch (priv->output_format) {
  case V4L2_PIX_FMT_MPEG:
  case V4L2_PIX_FMT_H264:
    if (priv->dw_mode_user_set) {
      GST_WARNING_OBJECT (sink, "overwrite user dw mode %d --> %d", priv->dw_mode, VDEC_DW_NO_AFBC);
      priv->dw_mode_user_set = FALSE;
    }
    priv->dw_mode = VDEC_DW_NO_AFBC;
    break;
  case V4L2_PIX_FMT_HEVC:
  case V4L2_PIX_FMT_VP9:
    if (priv->dw_mode_user_set) {
      GST_WARNING_OBJECT (sink, "enforce user dw mode %d", priv->dw_mode);
      break;
    }
    if (priv->es_width > 1920 || priv->es_width > 1080)
      priv->dw_mode = VDEC_DW_AFBC_1_2_DW;
    else
      priv->dw_mode = VDEC_DW_AFBC_1_1_DW;
    break;
  }

  /* HDR */
	if (gst_structure_has_field(structure, "colorimetry")) {
		const char *colorimetry = gst_structure_get_string (structure,"colorimetry");

		if (colorimetry &&
				sscanf( colorimetry, "%d:%d:%d:%d",
					&priv->hdr.Colorimetry[0],
					&priv->hdr.Colorimetry[1],
					&priv->hdr.Colorimetry[2],
					&priv->hdr.Colorimetry[3] ) == 4) {
			priv->hdr.haveColorimetry = TRUE;
			GST_INFO ("colorimetry: [%d,%d,%d,%d]",
					priv->hdr.Colorimetry[0],
					priv->hdr.Colorimetry[1],
					priv->hdr.Colorimetry[2],
					priv->hdr.Colorimetry[3]);
		}
	}

	if (gst_structure_has_field(structure, "mastering-display-metadata")) {
		const char *masteringDisplay = gst_structure_get_string (structure,"mastering-display-metadata");

		if (masteringDisplay &&
				sscanf( masteringDisplay, "%f:%f:%f:%f:%f:%f:%f:%f:%f:%f",
					&priv->hdr.MasteringDisplay[0],
					&priv->hdr.MasteringDisplay[1],
					&priv->hdr.MasteringDisplay[2],
					&priv->hdr.MasteringDisplay[3],
					&priv->hdr.MasteringDisplay[4],
					&priv->hdr.MasteringDisplay[5],
					&priv->hdr.MasteringDisplay[6],
					&priv->hdr.MasteringDisplay[7],
					&priv->hdr.MasteringDisplay[8],
					&priv->hdr.MasteringDisplay[9]) == 10) {
			priv->hdr.haveMasteringDisplay = TRUE;
			GST_INFO ("mastering display [%f,%f,%f,%f,%f,%f,%f,%f,%f,%f]",
					priv->hdr.MasteringDisplay[0],
					priv->hdr.MasteringDisplay[1],
					priv->hdr.MasteringDisplay[2],
					priv->hdr.MasteringDisplay[3],
					priv->hdr.MasteringDisplay[4],
					priv->hdr.MasteringDisplay[5],
					priv->hdr.MasteringDisplay[6],
					priv->hdr.MasteringDisplay[7],
					priv->hdr.MasteringDisplay[8],
					priv->hdr.MasteringDisplay[9]);
		}
	}

	if (gst_structure_has_field(structure, "content-light-level")) {
		const char *contentLightLevel = gst_structure_get_string (structure,"content-light-level");

		if (contentLightLevel &&
				sscanf(contentLightLevel, "%d:%d",
					&priv->hdr.ContentLightLevel[0],
					&priv->hdr.ContentLightLevel[1]) == 2) {
			GST_DEBUG ("content light level: [%d,%d])",
					priv->hdr.ContentLightLevel[0],
					priv->hdr.ContentLightLevel[1] );
			priv->hdr.haveContentLightLevel = TRUE;
		}
	}

  vsink_reset (sink);
  return TRUE;

error:
    return FALSE;
}

static inline void vsink_reset (GstAmlVsink * sink)
{
  GstAmlVsinkPrivate *priv = sink->priv;

  priv->eos = FALSE;
  priv->flushing_ = FALSE;
  priv->first_ts_set = FALSE;
  priv->output_port_config = FALSE;
  priv->output_start = FALSE;
}

static gboolean
gst_aml_vsink_event (GstAmlVsink *sink, GstEvent * event)
{
  gboolean result = TRUE;
  GstAmlVsinkPrivate *priv = sink->priv;
  GstBaseSink* bsink = GST_BASE_SINK_CAST (sink);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
    {
      int ret;
      struct v4l2_decoder_cmd cmd = {
        .cmd = V4L2_DEC_CMD_STOP,
        .flags = 0,
      };

      priv->received_eos = TRUE;
      GST_WARNING_OBJECT (sink, "EOS received");
      /* flush decoder */
      ret = ioctl(priv->fd, VIDIOC_DECODER_CMD, &cmd);
      if (ret)
        GST_ERROR_OBJECT (sink, "V4L2_DEC_CMD_STOP output fail %d",errno);

      GST_OBJECT_LOCK (sink);
      priv->eos = TRUE;
      GST_OBJECT_UNLOCK (sink);

      priv->seqnum = gst_event_get_seqnum (event);
      GST_DEBUG_OBJECT (sink, "eos seqnum #%" G_GUINT32_FORMAT, priv->seqnum);
      break;
    }
    case GST_EVENT_FLUSH_START:
    {
      GST_DEBUG_OBJECT (sink, "flush start");

      GST_OBJECT_LOCK (sink);
      priv->received_eos = FALSE;
      priv->flushing_ = TRUE;
      GST_OBJECT_UNLOCK (sink);
      break;
    }
    case GST_EVENT_FLUSH_STOP:
    {
      GST_DEBUG_OBJECT (sink, "flush stop");
      reset_decoder (sink);
      vsink_reset (sink);
#ifdef DUMP_TO_FILE
      file_index++;
#endif
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      gst_event_copy_segment (event, &priv->segment);
      GST_DEBUG_OBJECT (sink, "configured segment %" GST_SEGMENT_FORMAT,
          &priv->segment);

      /* prepare for trick play */
      if (priv->segment.rate != 1 && priv->segment.rate != 0) {
        //TODO: trickplay
      }
      break;
    }
    case GST_EVENT_STREAM_START:
    {
      guint group_id;

      gst_event_parse_group_id (event, &group_id);
      GST_DEBUG_OBJECT (sink, "group change from %d to %d",
          priv->group_id, group_id);
      priv->group_id = group_id;
      priv->group_done = FALSE;
      GST_DEBUG_OBJECT (sink, "stream start, gid %d", group_id);
      return GST_BASE_SINK_CLASS (parent_class)->event (bsink, event);
    }
    case GST_EVENT_STREAM_GROUP_DONE:
    {
      guint group_id;

      gst_event_parse_stream_group_done (event, &group_id);
      if (priv->group_id != group_id) {
        GST_WARNING_OBJECT (sink, "group id not match: %d vs %d",
            priv->group_id, group_id);
      } else {
        GST_DEBUG_OBJECT (sink, "stream group done, gid %d", group_id);
      }
      GST_OBJECT_LOCK (sink);
      priv->group_done = TRUE;
      GST_OBJECT_UNLOCK (sink);
    }
    default:
    {
      GST_DEBUG_OBJECT (sink, "pass to basesink");
      return GST_BASE_SINK_CLASS (parent_class)->event (bsink, event);
    }
  }
  gst_event_unref (event);
  return result;
}

static gboolean
gst_aml_vsink_pad_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstAmlVsink *sink = GST_AML_VSINK (parent);
  GstAmlVsinkPrivate *priv = sink->priv;
  gboolean result = TRUE;

  if (GST_EVENT_TYPE (event) != GST_EVENT_TAG)
    GST_DEBUG_OBJECT (sink, "received event %p %" GST_PTR_FORMAT, event, event);

  if (GST_EVENT_IS_SERIALIZED (event)) {
    if (G_UNLIKELY (priv->flushing_) &&
        GST_EVENT_TYPE (event) != GST_EVENT_FLUSH_STOP)
      goto flushing;

    if (G_UNLIKELY (priv->received_eos))
      goto after_eos;
  }

  result = gst_aml_vsink_event (sink, event);
done:
  return result;

  /* ERRORS */
flushing:
  {
    GST_DEBUG_OBJECT (sink, "we are flushing");
    gst_event_unref (event);
    result = FALSE;
    goto done;
  }

after_eos:
  {
    GST_DEBUG_OBJECT (sink, "Event received after EOS, dropping");
    gst_event_unref (event);
    result = FALSE;
    goto done;
  }
}

static void handle_v4l_event (GstAmlVsink *sink)
{
	int rc;
	struct v4l2_event event;
  GstAmlVsinkPrivate *priv = sink->priv;

	GST_OBJECT_LOCK (sink);

	memset (&event, 0, sizeof(event));

	rc= ioctl (priv->fd, VIDIOC_DQEVENT, &event);
  if (rc) {
    GST_ERROR ("fail VIDIOC_DQEVENT %d", errno);
    return;
  }

  if ( (event.type == V4L2_EVENT_SOURCE_CHANGE) &&
      (event.u.src_change.changes == V4L2_EVENT_SRC_CH_RESOLUTION) )
  {
    struct v4l2_selection selection;
    struct v4l2_format fmtOut;
    int32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    g_print("vsink: source change event\n");
    memset (&fmtOut, 0, sizeof(fmtOut));
    fmtOut.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rc = ioctl (priv->fd, VIDIOC_G_FMT, &fmtOut);
    if (rc) {
      GST_ERROR ("VIDIOC_G_FMT error %d", errno);
    }

    if (fmtOut.fmt.pix_mp.width != priv->coded_w ||
        fmtOut.fmt.pix_mp.height != priv->coded_h ||
        priv->out_frame_cnt > 0)
    {
      rc = ioctl (priv->fd, VIDIOC_STREAMOFF, &type);
      if (rc) {
        GST_ERROR ("cap VIDIOC_STREAMOFF error %d", errno);
        return;
      }

      priv->coded_w = fmtOut.fmt.pix_mp.width;
      priv->coded_h = fmtOut.fmt.pix_mp.height;

      pthread_mutex_lock (&priv->res_lock);
      recycle_capture_port_buffer (priv->fd, priv->cb, priv->cb_num);
      pthread_mutex_unlock (&priv->res_lock);

      if (v4l_dec_config(priv->fd, priv->secure,
            priv->output_format, priv->dw_mode,
            &priv->hdr)) {
        GST_ERROR("v4l_dec_config failed");
        return;
      }

      priv->cb = v4l_setup_capture_port (priv->fd, &priv->cb_num,
          priv->dw_mode, priv->render,
          &priv->coded_w, &priv->coded_h, priv->secure);
      if (!priv->cb) {
        GST_ERROR ("setup capture fail");
        return;
      }

      rc= ioctl (priv->fd, VIDIOC_STREAMON, &type);
      if ( rc < 0 )
      {
        GST_ERROR ("streamon failed for output: rc %d errno %d", rc, errno );
        return;
      }

      memset( &selection, 0, sizeof(selection) );
      selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      selection.target = V4L2_SEL_TGT_COMPOSE;
      rc = ioctl (priv->fd, VIDIOC_G_SELECTION, &selection);
      if (rc) {
        GST_ERROR ("fail to get visible dimension %d", errno);
      }
      priv->visible_w = selection.r.width;
      priv->visible_h = selection.r.height;
      GST_DEBUG ("visible %dx%d",  priv->visible_w, priv->visible_h);
    }
  } else if (event.type == V4L2_EVENT_EOS) {
    GstMessage * message;

    GST_WARNING_OBJECT (sink, "EOS received");
    priv->eos = TRUE;

    GST_DEBUG_OBJECT (sink, "Now posting EOS");
    message = gst_message_new_eos (GST_OBJECT_CAST (sink));
    gst_message_set_seqnum (message, priv->seqnum);
    gst_element_post_message (GST_ELEMENT_CAST (sink), message);
  }
	GST_OBJECT_UNLOCK (sink);
}

static struct capture_buffer* dqueue_capture_buffer(GstAmlVsink * sink)
{
  GstAmlVsinkPrivate *priv = sink->priv;
  struct v4l2_buffer buf;
  struct v4l2_plane planes[2];
  int ret;
  struct capture_buffer* cb;

  memset(&buf, 0, sizeof(buf));
  buf.memory = V4L2_MEMORY_DMABUF;
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  buf.m.planes = planes;
  buf.length = 2;
  ret = ioctl (priv->fd, VIDIOC_DQBUF, &buf);

  if (ret) {
    GST_ERROR ("cap VIDIOC_DQBUF fail %d", errno);
    return NULL;
  } else {
    cb = priv->cb[buf.index];
    cb->buf = buf;
    memcpy (&cb->plane, buf.m.planes, sizeof(struct v4l2_plane)*2);
    cb->buf.m.planes = cb->plane;
    cb->queued = false;
  }
  return cb;
}

static gpointer video_decode_thread(gpointer data)
{
  int rc;
  GstAmlVsink * sink = data;
  GstAmlVsinkPrivate *priv = sink->priv;
  struct v4l2_selection selection;
  uint32_t type;

  GST_DEBUG("enter");
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  rc= ioctl (priv->fd, VIDIOC_STREAMON, &type);
  if ( rc < 0 )
  {
    GST_ERROR("streamon failed for output: rc %d errno %d", rc, errno );
    goto exit;
  }

  memset( &selection, 0, sizeof(selection) );
  selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  selection.target = V4L2_SEL_TGT_COMPOSE;
  rc = ioctl (priv->fd, VIDIOC_G_SELECTION, &selection);
  if (rc) {
    GST_ERROR ("fail to get visible dimension %d", errno);
  }
  priv->visible_w = selection.r.width;
  priv->visible_h = selection.r.height;
  GST_DEBUG ("visible %dx%d",  priv->visible_w, priv->visible_h);

  while (!priv->quitVideoOutputThread) {
    gint64 frame_ts;
    struct capture_buffer *cb;
    struct pollfd pfd = {
        /* default blocking capture */
        .events =  POLLIN | POLLRDNORM | POLLPRI,
        .fd = priv->fd,
        .revents= 0,
    };

    for (;;) {
      int ret;

      ret = poll (&pfd, 1, 10);
      if (ret > 0)
        break;
      if (priv->quitVideoOutputThread)
        goto exit;
      if (errno == EINTR)
        continue;

    }

    if (pfd.revents & POLLPRI) {
      handle_v4l_event (sink);
      continue;
    }

    /* only handle capture port */
    if ((pfd.revents & (POLLIN|POLLRDNORM)) == 0) {
      usleep(1000);
      continue;
    }

    cb = dqueue_capture_buffer (sink);
    if (!cb) {
      GST_WARNING_OBJECT (sink, "capture buf not available");
      continue;
    }

    frame_ts = GST_TIMEVAL_TO_TIME(cb->buf.timestamp);
    if (frame_ts < priv->segment.start) {
      GST_INFO ("drop frame %lld before start %lld", frame_ts, priv->segment.start);
      v4l_queue_capture_buffer (priv->fd, cb);
      continue;
    }

    priv->position = priv->segment.start + (frame_ts - priv->first_ts);

    if (priv->out_frame_cnt == 0) {
      GST_DEBUG("emit first frame signal");
      g_signal_emit (G_OBJECT (sink), g_signals[SIGNAL_FIRSTFRAME], 0, 2, NULL);
    }

    priv->out_frame_cnt++;

    if (priv->fr)
      cb->drm_frame->duration = 90000/priv->fr;
    else
      cb->drm_frame->duration = 0;

    //TODO: handle scale_set in drm
    display_engine_show (priv->render, cb->drm_frame, &priv->window);
  }

exit:
    /* stop output port */
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    rc = ioctl (priv->fd, VIDIOC_STREAMOFF, &type);
    if (rc)
        GST_ERROR ("VIDIOC_STREAMOFF fail ret:%d\n",rc);

    /* stop capture port */
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rc = ioctl (priv->fd, VIDIOC_STREAMOFF, &type);
    if (rc)
        GST_ERROR ("VIDIOC_STREAMOFF fail ret:%d\n",rc);

    return NULL;
}

static int start_video_thread (GstAmlVsink * sink)
{
  GstAmlVsinkPrivate *priv = sink->priv;

  priv->out_frame_cnt = 0;
  priv->dropped_frame_num = 0;

  priv->quitVideoOutputThread = FALSE;
  if (!priv->videoOutputThread) {
    GST_DEBUG_OBJECT (sink, "starting video thread");
    priv->videoOutputThread = g_thread_new ("video output thread", video_decode_thread, sink);
  }

  priv->output_start = TRUE;
  return 0;
}

static int get_output_buffer(GstAmlVsink * sink)
{
  GstAmlVsinkPrivate *priv = sink->priv;
	int index= -1;
	int i;

	for (i= 0; i < priv->ob_num; ++i) {
		if (!priv->ob[i]->queued) {
			index= i;
			break;
		}
	}

	if (index < 0) {
		int rc;
		struct v4l2_buffer buf;
		struct v4l2_plane plane;

		memset (&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		buf.memory = priv->output_mode;
		buf.length = 1;
		buf.m.planes = &plane;
		rc = ioctl ( priv->fd, VIDIOC_DQBUF, &buf );
		if (!rc) {
      index = buf.index;
			priv->ob[index]->plane = plane;
			priv->ob[index]->buf = buf;
			priv->ob[index]->queued = false;
		}
	}
	return index;
}

static GstFlowReturn decode_buf (GstAmlVsink * sink, GstBuffer * buf)
{
  GstAmlVsinkPrivate *priv = sink->priv;
  GstMemory *mem;
  gsize inSize;
  int index;
  int rc;
  struct output_buffer *ob;

  mem = gst_buffer_peek_memory (buf, 0);
  if (!priv->output_port_config) {
    if (gst_is_dmabuf_memory (mem))
      priv->output_mode = V4L2_MEMORY_DMABUF;
    else
      priv->output_mode = V4L2_MEMORY_MMAP;

    priv->secure = (priv->output_mode == V4L2_MEMORY_DMABUF);
    rc = v4l_set_secure_mode (priv->fd, priv->es_width,
        priv->es_height, priv->secure);
    if (rc) {
      GST_ERROR_OBJECT (sink, "set secure mode fail");
      return GST_FLOW_ERROR;
    }

    rc = v4l_set_output_format (priv->fd, priv->output_format,
        priv->es_width, priv->es_height);
    if (rc) {
      GST_ERROR_OBJECT (sink, "set output format %x fail", priv->output_format);
      return GST_FLOW_ERROR;
    }

    priv->ob = v4l_setup_output_port (priv->fd, priv->output_mode, &priv->ob_num);
    if (!priv->ob) {
      GST_ERROR_OBJECT (sink, "setup output fail");
      return GST_FLOW_ERROR;
    }
    priv->output_port_config = TRUE;
  }

  if (GST_BUFFER_PTS_IS_VALID(buf)) {
    if (!priv->first_ts_set) {
      priv->first_ts = GST_BUFFER_PTS (buf);
      priv->first_ts_set = true;
    }
  }

  index = get_output_buffer (sink);
  if (index < 0) {
    GST_ERROR ("can not get output buffer");
    goto exit;
  }
  ob = priv->ob[index];

  if (priv->output_mode == V4L2_MEMORY_DMABUF) {
    gsize dataOffset, maxSize;

    if (ob->gstbuf) {
      gst_buffer_unref (ob->gstbuf);
      ob->gstbuf = NULL;
    }

    if (GST_BUFFER_PTS_IS_VALID (buf))
      GST_TIME_TO_TIMEVAL (GST_BUFFER_PTS(buf), ob->buf.timestamp);

    inSize= gst_memory_get_sizes( mem, &dataOffset, &maxSize );

    ob->buf.bytesused = dataOffset+inSize;
    ob->buf.m.planes[0].m.fd = gst_dmabuf_memory_get_fd(mem);
    ob->buf.m.planes[0].bytesused = dataOffset+inSize;
    ob->buf.m.planes[0].length = dataOffset+inSize;
    ob->buf.m.planes[0].data_offset = dataOffset;

    rc = ioctl (priv->fd, VIDIOC_QBUF, &ob->buf );
    if (rc) {
      GST_ERROR ("queuing output buffer failed: rc %d errno %d", rc, errno );
      goto exit;
    }
    ob->queued = TRUE;
    ob->gstbuf = gst_buffer_ref (buf);
  } else if (priv->output_mode == V4L2_MEMORY_MMAP) {
    GstMapInfo map;
    guint8 * inData;

    gst_buffer_map (buf, &map, (GstMapFlags)GST_MAP_READ);
    inSize = map.size;
    inData = map.data;

    if (inSize) {
      gsize copylen;

      if ( priv->flushing_) {
        GST_WARNING_OBJECT (sink, "drop frame in flushing");
        goto exit;
      }

      copylen = ob->size;
      if (copylen >= inSize)
        copylen = inSize;
      else
        GST_WARNING_OBJECT (sink, "sample too big %d vs %d", copylen, inSize);

      memcpy (ob->vaddr, inData, copylen);

      if (GST_BUFFER_PTS_IS_VALID(buf))
        GST_TIME_TO_TIMEVAL(GST_BUFFER_PTS(buf), ob->buf.timestamp);

      ob->buf.bytesused = copylen;
      ob->buf.m.planes[0].bytesused = copylen;
      rc = ioctl (priv->fd, VIDIOC_QBUF, &ob->buf);
      if (rc) {
        GST_ERROR("queuing output buffer failed: rc %d errno %d", rc, errno);
        goto exit;
      }
      ob->queued = true;
      GST_DEBUG_OBJECT (sink, "queue ob %d len %d ts %lld", ob->buf.index, copylen, GST_BUFFER_PTS(buf));
#ifdef DUMP_TO_FILE
      if (getenv("AML_VSINK_ES_DUMP"))
        dump ("/data/amlvsink", inData, copylen,
            priv->output_format == V4L2_PIX_FMT_VP9,
            priv->in_frame_cnt == 0);
#endif
    }
    gst_buffer_unmap (buf, &map);
  }

  priv->in_frame_cnt++;

  if (!priv->output_start) {
    uint32_t type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

    GST_DEBUG ("output VIDIOC_STREAMON");
    rc= ioctl (priv->fd, VIDIOC_STREAMON, &type);
    if (rc) {
      GST_ERROR ("streamon failed for output: rc %d errno %d", rc, errno );
      return GST_FLOW_ERROR;
    }

    if (v4l_dec_config (priv->fd, priv->secure,
          priv->output_format, priv->dw_mode, &priv->hdr)) {
      GST_ERROR("v4l_dec_config failed");
      return GST_FLOW_ERROR;
    }

    priv->cb = v4l_setup_capture_port (priv->fd, &priv->cb_num,
        priv->dw_mode, priv->render,
        &priv->coded_w, &priv->coded_h, priv->secure);
    if (!priv->cb) {
      GST_ERROR_OBJECT (sink, "setup capture fail");
      return GST_FLOW_ERROR;
    }

    if (start_video_thread (sink)) {
      GST_ERROR("start_video_thread failed");
      return GST_FLOW_ERROR;
    }
  }

exit:
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_aml_vsink_render (GstAmlVsink * sink, GstBuffer * buf)
{
  GstClockTime time, duration;
  GstAmlVsinkPrivate *priv = sink->priv;
  GstFlowReturn ret = GST_FLOW_OK;

  if (priv->flushing_) {
    ret = GST_FLOW_FLUSHING;
    goto done;
  }

  if (G_UNLIKELY (priv->received_eos))
    goto was_eos;

  time = GST_BUFFER_TIMESTAMP (buf);
  duration= GST_BUFFER_DURATION(buf);
  if (!GST_CLOCK_TIME_IS_VALID(duration)) {
    if (priv->fr != 0) {
      duration= GST_SECOND / priv->fr;
      GST_BUFFER_DURATION(buf)= duration;
    }
  }
  if (GST_CLOCK_TIME_IS_VALID(duration) &&
    (time + duration < priv->segment.start)) {
    GST_DEBUG_OBJECT (sink, "out of sement sample %lld, start %lld, key %d",
      time, priv->segment.start,
      !GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT));
  } else {
    GST_LOG_OBJECT (sink,
        "time %lld, start %lld, duration %lld key %d",
        time, priv->segment.start, duration,
        !GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT));
  }

  decode_buf (sink, buf);

  ret = GST_FLOW_OK;
done:
  gst_buffer_unref (buf);
  return ret;

was_eos:
  {
    GST_DEBUG_OBJECT (sink, "we are EOS, return EOS");
    ret = GST_FLOW_EOS;
    goto done;
  }
  /* SPECIAL cases */
}

static GstFlowReturn
gst_aml_vsink_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstAmlVsink *sink = GST_AML_VSINK (parent);

  return gst_aml_vsink_render (sink, buf);
}

static GstStateChangeReturn ready_to_pause(GstAmlVsink *sink)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_FAILURE;
  GstAmlVsinkPrivate *priv = sink->priv;
  int fd;
  uint32_t fnum;
  struct v4l2_fmtdesc *formats = NULL;
  int rc;

  fd = v4l_dec_open();
  if (fd < 0) {
    GST_ERROR("dec open fail");
    goto error;
  }

  /* output port formats */
  formats = v4l_get_output_port_formats (fd, &fnum);
  if (!formats)
    goto error;

  priv->output_formats = formats;

  /* capture port formats */
  formats = v4l_get_capture_port_formats (fd, &fnum);
  if (!formats) {
    GST_ERROR("get capture format fail");
    goto error;
  }

  priv->capture_formats = formats;

  rc = v4l_reg_event(fd);
  if (rc) {
    GST_ERROR("reg event fail");
    goto error;
  }

  priv->fd = fd;
#ifdef DUMP_TO_FILE
  file_index++;
#endif

  /* render init */
  priv->render = display_engine_start(priv);
  display_engine_register_cb(capture_buffer_recycle);
  if (!priv->render)
    goto error;

  return GST_STATE_CHANGE_SUCCESS;
error:
  if (fd >= 0 )
    close( fd );

  if (priv->output_formats)
    g_free (priv->output_formats);

  if (priv->capture_formats)
    g_free (priv->capture_formats);

  return ret;

}

static void reset_decoder(GstAmlVsink *sink)
{
  int ret;
  uint32_t type;
  GstAmlVsinkPrivate *priv = sink->priv;

  priv->quitVideoOutputThread= TRUE;

  /* stop output port */
  type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  ret = ioctl(priv->fd, VIDIOC_STREAMOFF, &type);
  if (ret) {
    GST_ERROR ("VIDIOC_STREAMOFF fail ret:%d\n",ret);
  }

  recycle_output_port_buffer (priv->fd, priv->ob, priv->ob_num);

  /* stop capture port */
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  ret = ioctl(priv->fd, VIDIOC_STREAMOFF, &type);
  if (ret) {
    GST_ERROR ("VIDIOC_STREAMOFF fail ret:%d\n",ret);
  }

  recycle_capture_port_buffer (priv->fd, priv->cb, priv->cb_num);
  g_thread_join (priv->videoOutputThread);
}

static GstStateChangeReturn pause_to_ready(GstAmlVsink *sink)
{
  GstAmlVsinkPrivate *priv = sink->priv;

  display_engine_stop (priv->render);

  v4l_unreg_event (priv->fd);

  reset_decoder (sink);

  if (priv->fd > 0) {
    close (priv->fd);
    priv->fd = -1;
  }
  vsink_reset (sink);
  return GST_STATE_CHANGE_SUCCESS;
}

static GstStateChangeReturn
gst_aml_vsink_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstAmlVsink *sink = GST_AML_VSINK (element);
  GstAmlVsinkPrivate *priv = sink->priv;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
    {
      GST_DEBUG_OBJECT(sink, "null to ready");
      break;
    }
    case GST_STATE_CHANGE_READY_TO_PAUSED:
    {
      GST_DEBUG_OBJECT(sink, "ready to paused");
      ready_to_pause (sink);
      priv->paused = TRUE;
      display_set_pause (priv->render, true);
      break;
    }
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
    {
      GST_DEBUG_OBJECT(sink, "paused to playing");
      if (priv->paused) {
        display_set_pause (priv->render, false);
        priv->paused = FALSE;
      }
      break;
    }
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
    {
      GST_DEBUG_OBJECT(sink, "playing to paused");
      if (!priv->paused) {
        display_set_pause (priv->render, true);
        priv->paused = TRUE;
      }
      display_set_pause (priv->render, true);
      break;
    }
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_DEBUG_OBJECT(sink, "paused to ready");
      pause_to_ready (sink);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
      GST_DEBUG_OBJECT(sink, "ready to null");
      break;
    default:
      break;
  }

  return ret;
}

int capture_buffer_recycle(void* priv_data, void* handle)
{
  int ret = 0;
  struct capture_buffer *frame = handle;
  GstAmlVsinkPrivate *priv = priv_data;

  if (!frame->drm_frame->displayed)
    priv->dropped_frame_num++;

  pthread_mutex_lock (&priv->res_lock);
  if (frame->free_on_recycle) {
    GST_DEBUG ("free index:%d\n", frame->buf.index);
    frame->drm_frame->destroy(frame->drm_frame);
    free(frame);
    goto exit;
  }

  if (priv->eos)
    goto exit;

  ret = v4l_queue_capture_buffer(priv->fd, frame);
  if (ret) {
    GST_ERROR ("queue cap fail %d", frame->id);
  }

exit:
  pthread_mutex_unlock (&priv->res_lock);
  return ret;
}

#if 0
static int get_sysfs_uint32(const char *path, uint32_t *value)
{
    int fd;
    char valstr[64];
    uint32_t val = 0;

    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        memset(valstr, 0, 64);
        read(fd, valstr, 64 - 1);
        valstr[strlen(valstr)] = '\0';
        close(fd);
    } else {
        GST_ERROR("unable to open file %s", path);
        return -1;
    }
    if (sscanf(valstr, "0x%x", &val) < 1) {
        GST_ERROR("unable to get pts from: %s", valstr);
        return -1;
    }
    *value = val;
    return 0;
}

static int config_sys_node(const char* path, const char* value)
{
  int fd;
  fd = open(path, O_RDWR);
  if (fd < 0) {
    GST_ERROR("fail to open %s", path);
    return -1;
  }
  if (write(fd, value, strlen(value)) != strlen(value)) {
    GST_ERROR("fail to write %s to %s", value, path);
    close(fd);
    return -1;
  }
  close(fd);

  return 0;
}
#endif

#ifdef DUMP_TO_FILE
static uint8_t ivf_header[32] = {
  'D', 'K', 'I', 'F',
  0x00, 0x00, 0x20, 0x00,
  'V', 'P', '9', '0',
  0x80, 0x07, 0x38, 0x04, /* 1920 x 1080 */
  0x30, 0x76, 0x00, 0x00, /* frame rate */
  0xe8, 0x03, 0x00, 0x00, /* time scale */
  0x00, 0x00, 0xff, 0xff, /* # of frames */
  0x00, 0x00, 0x00, 0x00  /* unused */
};

static void dump(const char* path, const uint8_t *data, int size, gboolean vp9, int frame_cnt)
{
  char name[50];
  uint8_t frame_header[12] = {0};
  FILE* fd;

  sprintf(name, "%s%d.dat", path, file_index);
  fd = fopen(name, "ab");

  if (!fd)
    return;
  if (vp9) {
    if (!frame_cnt)
      fwrite(ivf_header, 1, 32, fd);
    frame_header[0] = size & 0xff;
    frame_header[1] = (size >> 8) & 0xff;
    frame_header[2] = (size >> 16) & 0xff;
    frame_header[3] = (size >> 24) & 0xff;
    fwrite(frame_header, 1, 12, fd);
  }
  fwrite(data, 1, size, fd);
  fclose(fd);
}
#endif

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "amlvsink", GST_RANK_PRIMARY,
      GST_TYPE_AML_VSINK);
}

#ifndef VERSION
#define VERSION "0.1.0"
#endif
#ifndef PACKAGE
#define PACKAGE "aml_package"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "aml_media"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "http://amlogic.com"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    amlvsink,
    "Amlogic plugin for video decoding/rendering",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
