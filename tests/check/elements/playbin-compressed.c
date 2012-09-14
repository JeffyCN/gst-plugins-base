/* GStreamer unit tests for playbin compressed stream support
 *
 * Copyright (C) 2006 Tim-Philipp Müller <tim centricular net>
 * Copyright (C) 2010 Sebastian Dröge <sebastian.droege@collabora.co.uk>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <gst/check/gstcheck.h>
#include <gst/base/gstpushsrc.h>
#include <gst/base/gstbasesink.h>
#include <gst/audio/streamvolume.h>

#ifndef GST_DISABLE_REGISTRY

#define NBUFFERS 100

static GType gst_caps_src_get_type (void);
static GType gst_codec_demuxer_get_type (void);
static GType gst_codec_sink_get_type (void);
static GType gst_audio_codec_sink_get_type (void);
static GType gst_video_codec_sink_get_type (void);

typedef struct _GstCapsSrc GstCapsSrc;
typedef GstPushSrcClass GstCapsSrcClass;

struct _GstCapsSrc
{
  GstPushSrc parent;

  GstCaps *caps;
  gchar *uri;
  gint nbuffers;
};

static GstURIType
gst_caps_src_uri_get_type (GType type)
{
  return GST_URI_SRC;
}

static const gchar *const *
gst_caps_src_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { "caps", NULL };

  return protocols;
}

static gchar *
gst_caps_src_uri_get_uri (GstURIHandler * handler)
{
  GstCapsSrc *src = (GstCapsSrc *) handler;

  return g_strdup (src->uri);
}

static gboolean
gst_caps_src_uri_set_uri (GstURIHandler * handler, const gchar * uri,
    GError ** error)
{
  GstCapsSrc *src = (GstCapsSrc *) handler;

  if (uri == NULL || !g_str_has_prefix (uri, "caps:"))
    return FALSE;

  g_free (src->uri);
  src->uri = g_strdup (uri);

  if (src->caps)
    gst_caps_unref (src->caps);
  src->caps = NULL;

  return TRUE;
}

static void
gst_caps_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_caps_src_uri_get_type;
  iface->get_protocols = gst_caps_src_uri_get_protocols;
  iface->get_uri = gst_caps_src_uri_get_uri;
  iface->set_uri = gst_caps_src_uri_set_uri;
}

G_DEFINE_TYPE_WITH_CODE (GstCapsSrc, gst_caps_src, GST_TYPE_PUSH_SRC,
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER,
        gst_caps_src_uri_handler_init));

static void
gst_caps_src_finalize (GObject * object)
{
  GstCapsSrc *src = (GstCapsSrc *) object;

  if (src->caps)
    gst_caps_unref (src->caps);
  src->caps = NULL;
  g_free (src->uri);

  G_OBJECT_CLASS (gst_caps_src_parent_class)->finalize (object);
}

static GstFlowReturn
gst_caps_src_create (GstPushSrc * psrc, GstBuffer ** p_buf)
{
  GstCapsSrc *src = (GstCapsSrc *) psrc;
  GstBuffer *buf;

  if (src->nbuffers >= NBUFFERS) {
    return GST_FLOW_EOS;
  }

  if (!src->caps) {
    if (!src->uri) {
      return GST_FLOW_ERROR;
    }

    src->caps = gst_caps_from_string (src->uri + sizeof ("caps"));
    if (!src->caps) {
      return GST_FLOW_ERROR;
    }
  }

  if (src->nbuffers == 0) {
    gst_pad_set_caps (GST_BASE_SRC_PAD (psrc), src->caps);
  }

  buf = gst_buffer_new ();
  GST_BUFFER_TIMESTAMP (buf) =
      gst_util_uint64_scale (src->nbuffers, GST_SECOND, 25);
  src->nbuffers++;

  *p_buf = buf;
  return GST_FLOW_OK;
}

static void
gst_caps_src_class_init (GstCapsSrcClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstPushSrcClass *pushsrc_class = (GstPushSrcClass *) klass;
  static GstStaticPadTemplate src_templ = GST_STATIC_PAD_TEMPLATE ("src",
      GST_PAD_SRC, GST_PAD_ALWAYS,
      GST_STATIC_CAPS_ANY);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_templ));
  gst_element_class_set_details_simple (element_class,
      "CapsSource", "Source/Generic", "yep", "me");

  gobject_class->finalize = gst_caps_src_finalize;
  pushsrc_class->create = gst_caps_src_create;
}

static void
gst_caps_src_init (GstCapsSrc * src)
{
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
}

typedef struct _GstCodecSink GstCodecSink;
typedef GstBaseSinkClass GstCodecSinkClass;

struct _GstCodecSink
{
  GstBaseSink parent;

  gboolean audio;
  gboolean raw;
  gint n_raw, n_compressed;
};

G_DEFINE_TYPE (GstCodecSink, gst_codec_sink, GST_TYPE_BASE_SINK);

static gboolean
gst_codec_sink_start (GstBaseSink * bsink)
{
  GstCodecSink *sink = (GstCodecSink *) bsink;

  sink->n_raw = 0;
  sink->n_compressed = 0;

  return TRUE;
}

static GstFlowReturn
gst_codec_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstCodecSink *sink = (GstCodecSink *) bsink;

  if (sink->raw)
    sink->n_raw++;
  else
    sink->n_compressed++;

  return GST_FLOW_OK;
}

static void
gst_codec_sink_class_init (GstCodecSinkClass * klass)
{
  GstBaseSinkClass *basesink_class = (GstBaseSinkClass *) klass;

  basesink_class->start = gst_codec_sink_start;
  basesink_class->render = gst_codec_sink_render;
}

static void
gst_codec_sink_init (GstCodecSink * sink)
{
}

typedef GstCodecSink GstAudioCodecSink;
typedef GstCodecSinkClass GstAudioCodecSinkClass;

G_DEFINE_TYPE_WITH_CODE (GstAudioCodecSink, gst_audio_codec_sink,
    gst_codec_sink_get_type (),
    G_IMPLEMENT_INTERFACE (GST_TYPE_STREAM_VOLUME, NULL));

static void
gst_audio_codec_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    case 1:
    case 2:
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_audio_codec_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{

  switch (prop_id) {
    case 1:
      g_value_set_double (value, 1.0);
      break;
    case 2:
      g_value_set_boolean (value, FALSE);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_audio_codec_sink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstAudioCodecSink *sink = (GstAudioCodecSink *) bsink;
  GstStructure *s;

  s = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_name (s, "audio/x-raw")) {
    sink->raw = TRUE;
  } else if (gst_structure_has_name (s, "audio/x-compressed")) {
    sink->raw = FALSE;
  } else {
    fail_unless (gst_structure_has_name (s, "audio/x-raw")
        || gst_structure_has_name (s, "audio/x-compressed"));
    return FALSE;
  }

  return TRUE;
}

static void
gst_audio_codec_sink_class_init (GstAudioCodecSinkClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *basesink_class = (GstBaseSinkClass *) klass;
  static GstStaticPadTemplate sink_templ = GST_STATIC_PAD_TEMPLATE ("sink",
      GST_PAD_SINK, GST_PAD_ALWAYS,
      GST_STATIC_CAPS ("audio/x-raw; audio/x-compressed")
      );

  gobject_class->set_property = gst_audio_codec_sink_set_property;
  gobject_class->get_property = gst_audio_codec_sink_get_property;

  g_object_class_install_property (gobject_class,
      1,
      g_param_spec_double ("volume", "Volume",
          "Linear volume of this stream, 1.0=100%", 0.0, 10.0,
          1.0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
      2,
      g_param_spec_boolean ("mute", "Mute",
          "Mute", FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_templ));
  gst_element_class_set_details_simple (element_class,
      "AudioCodecSink", "Sink/Audio", "yep", "me");

  basesink_class->set_caps = gst_audio_codec_sink_set_caps;
}

static void
gst_audio_codec_sink_init (GstAudioCodecSink * sink)
{
  sink->audio = TRUE;
}

typedef GstCodecSink GstVideoCodecSink;
typedef GstCodecSinkClass GstVideoCodecSinkClass;

G_DEFINE_TYPE (GstVideoCodecSink, gst_video_codec_sink,
    gst_codec_sink_get_type ());

static gboolean
gst_video_codec_sink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstVideoCodecSink *sink = (GstVideoCodecSink *) bsink;
  GstStructure *s;

  s = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_name (s, "video/x-raw")) {
    sink->raw = TRUE;
  } else if (gst_structure_has_name (s, "video/x-compressed")) {
    sink->raw = FALSE;
  } else {
    fail_unless (gst_structure_has_name (s, "video/x-raw")
        || gst_structure_has_name (s, "video/x-compressed"));
    return FALSE;
  }

  return TRUE;
}

static void
gst_video_codec_sink_class_init (GstVideoCodecSinkClass * klass)
{
  GstBaseSinkClass *basesink_class = (GstBaseSinkClass *) klass;
  static GstStaticPadTemplate sink_templ = GST_STATIC_PAD_TEMPLATE ("sink",
      GST_PAD_SINK, GST_PAD_ALWAYS,
      GST_STATIC_CAPS ("video/x-raw; video/x-compressed")
      );
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_templ));
  gst_element_class_set_details_simple (element_class,
      "VideoCodecSink", "Sink/Video", "yep", "me");

  basesink_class->set_caps = gst_video_codec_sink_set_caps;
}

static void
gst_video_codec_sink_init (GstVideoCodecSink * sink)
{
  sink->audio = FALSE;
}

typedef struct _GstCodecDemuxer GstCodecDemuxer;
typedef GstElementClass GstCodecDemuxerClass;

struct _GstCodecDemuxer
{
  GstElement parent;

  GstPad *sinkpad;
  GstPad *srcpad0, *srcpad1;

  GstEvent *newseg_event;
};

#define STREAM_TYPES "{ " \
    "none, " \
    "raw-audio, " \
    "compressed-audio, " \
    "raw-video, " \
    "compressed-video " \
    "}"

static GstStaticPadTemplate cd_sink_templ = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-container,"
        " stream0 = (string)" STREAM_TYPES " ,"
        " stream1 = (string)" STREAM_TYPES)
    );
static GstStaticPadTemplate cd_src_templ = GST_STATIC_PAD_TEMPLATE ("src_%d",
    GST_PAD_SRC, GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("audio/x-raw; audio/x-compressed; "
        "video/x-raw; video/x-compressed")
    );

G_DEFINE_TYPE (GstCodecDemuxer, gst_codec_demuxer, GST_TYPE_ELEMENT);

static void
gst_codec_demuxer_finalize (GObject * object)
{
  GstCodecDemuxer *demux = (GstCodecDemuxer *) object;

  if (demux->newseg_event)
    gst_event_unref (demux->newseg_event);
  demux->newseg_event = NULL;

  G_OBJECT_CLASS (gst_codec_demuxer_parent_class)->finalize (object);
}

static void
gst_codec_demuxer_class_init (GstCodecDemuxerClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->finalize = gst_codec_demuxer_finalize;

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&cd_sink_templ));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&cd_src_templ));
  gst_element_class_set_details_simple (element_class,
      "CodecDemuxer", "Codec/Demuxer", "yep", "me");
}

static GstFlowReturn
gst_codec_demuxer_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstCodecDemuxer *demux = (GstCodecDemuxer *) parent;
  GstFlowReturn ret0 = GST_FLOW_OK, ret1 = GST_FLOW_OK;

  if (demux->srcpad0) {
    GstBuffer *outbuf = gst_buffer_new ();

    GST_BUFFER_TIMESTAMP (outbuf) = GST_BUFFER_TIMESTAMP (buf);
    ret0 = gst_pad_push (demux->srcpad0, outbuf);
  }
  if (demux->srcpad1) {
    GstBuffer *outbuf = gst_buffer_new ();

    GST_BUFFER_TIMESTAMP (outbuf) = GST_BUFFER_TIMESTAMP (buf);
    ret1 = gst_pad_push (demux->srcpad1, outbuf);
  }
  gst_buffer_unref (buf);

  if (ret0 == GST_FLOW_NOT_LINKED && ret1 == GST_FLOW_NOT_LINKED)
    return GST_FLOW_NOT_LINKED;
  if (ret0 == GST_FLOW_OK && ret1 == GST_FLOW_OK)
    return GST_FLOW_OK;

  return MIN (ret0, ret1);
}

static void
gst_codec_demuxer_setup_pad (GstCodecDemuxer * demux, GstPad ** pad,
    const gchar * streaminfo)
{
  if (g_str_equal (streaminfo, "none")) {
    if (*pad) {
      gst_pad_set_active (*pad, FALSE);
      gst_element_remove_pad (GST_ELEMENT (demux), *pad);
      *pad = NULL;
    }
  } else {
    GstCaps *caps;

    if (!*pad) {
      GstPadTemplate *templ;

      templ =
          gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (demux),
          "src_%d");
      if (pad == &demux->srcpad0)
        *pad = gst_pad_new_from_template (templ, "src_0");
      else
        *pad = gst_pad_new_from_template (templ, "src_1");
      gst_pad_set_active (*pad, TRUE);
      gst_pad_use_fixed_caps (*pad);
      gst_element_add_pad (GST_ELEMENT (demux), *pad);
    }

    if (g_str_equal (streaminfo, "raw-video")) {
      caps = gst_caps_new_simple ("video/x-raw",
          "format", G_TYPE_STRING, "I420",
          "width", G_TYPE_INT, 320,
          "height", G_TYPE_INT, 240,
          "framerate", GST_TYPE_FRACTION, 25, 1,
          "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
    } else if (g_str_equal (streaminfo, "compressed-video")) {
      caps = gst_caps_new_empty_simple ("video/x-compressed");
    } else if (g_str_equal (streaminfo, "raw-audio")) {
      caps = gst_caps_new_simple ("audio/x-raw",
          "format", G_TYPE_STRING, "S16LE",
          "layout", G_TYPE_STRING, "interleaved",
          "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 2, NULL);
    } else {
      caps = gst_caps_new_empty_simple ("audio/x-compressed");
    }
    gst_pad_set_caps (*pad, caps);
    gst_caps_unref (caps);

    if (demux->newseg_event)
      gst_pad_push_event (*pad, gst_event_ref (demux->newseg_event));
  }
}

static gboolean
gst_codec_demuxer_setcaps (GstPad * pad, GstObject * parent, GstCaps * caps)
{
  GstCodecDemuxer *demux = (GstCodecDemuxer *) parent;
  GstStructure *s;
  const gchar *streaminfo;

  s = gst_caps_get_structure (caps, 0);

  streaminfo = gst_structure_get_string (s, "stream0");
  gst_codec_demuxer_setup_pad (demux, &demux->srcpad0, streaminfo);

  streaminfo = gst_structure_get_string (s, "stream1");
  gst_codec_demuxer_setup_pad (demux, &demux->srcpad1, streaminfo);

  return TRUE;
}

static gboolean
gst_codec_demuxer_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstCodecDemuxer *demux = (GstCodecDemuxer *) parent;
  gboolean ret = TRUE;

  /* The single newsegment event is pushed when the pads are created */
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_codec_demuxer_setcaps (pad, parent, caps);
      break;
    }
    case GST_EVENT_SEGMENT:
      gst_event_replace (&demux->newseg_event, event);
      break;
    default:
      if (demux->srcpad0)
        ret = ret && gst_pad_push_event (demux->srcpad0, gst_event_ref (event));
      if (demux->srcpad1)
        ret = ret && gst_pad_push_event (demux->srcpad1, gst_event_ref (event));
      break;
  }
  gst_event_unref (event);

  return ret;
}

static void
gst_codec_demuxer_init (GstCodecDemuxer * demux)
{
  demux->sinkpad = gst_pad_new_from_static_template (&cd_sink_templ, "sink");
  gst_pad_set_chain_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_codec_demuxer_chain));
  gst_pad_set_event_function (demux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_codec_demuxer_event));
  gst_element_add_pad (GST_ELEMENT (demux), demux->sinkpad);
}

/****
 * Start of the tests
 ***/

static GstElement *
create_playbin (const gchar * uri, gboolean set_sink)
{
  GstElement *playbin, *sink;

  playbin = gst_element_factory_make ("playbin", "playbin");
  fail_unless (playbin != NULL, "Failed to create playbin element");

  if (set_sink) {
    sink = gst_element_factory_make ("videocodecsink", NULL);
    fail_unless (sink != NULL, "Failed to create videocodecsink element");

    /* Set sync to FALSE to prevent buffers from being dropped because
     * they're too late */
    g_object_set (sink, "sync", FALSE, NULL);

    g_object_set (playbin, "video-sink", sink, NULL);

    sink = gst_element_factory_make ("audiocodecsink", NULL);
    fail_unless (sink != NULL, "Failed to create audiocodecsink");

    /* Set sync to FALSE to prevent buffers from being dropped because
     * they're too late */
    g_object_set (sink, "sync", FALSE, NULL);

    g_object_set (playbin, "audio-sink", sink, NULL);
  }

  g_object_set (playbin, "uri", uri, NULL);

  return playbin;
}

GST_START_TEST (test_raw_single_video_stream_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin =
      create_playbin
      ("caps:video/x-raw, "
      "format=(string)I420, "
      "width=(int)320, "
      "height=(int)240, "
      "framerate=(fraction)0/1, " "pixel-aspect-ratio=(fraction)1/1", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->raw == TRUE);
    fail_unless_equals_int (csink->n_raw, NBUFFERS);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_compressed_single_video_stream_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:video/x-compressed", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, NBUFFERS);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_raw_single_video_stream_demuxer_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)raw-video, " "stream1=(string)none", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->raw == TRUE);
    fail_unless_equals_int (csink->n_raw, NBUFFERS);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_compressed_single_video_stream_demuxer_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)compressed-video, " "stream1=(string)none", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, NBUFFERS);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_raw_single_audio_stream_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin =
      create_playbin
      ("caps:audio/x-raw,"
      " format=(string)S16LE, " "layout=(string)interleaved, "
      " rate=(int)48000, " " channels=(int)2", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->raw == TRUE);
    fail_unless_equals_int (csink->n_raw, NBUFFERS);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_compressed_single_audio_stream_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:audio/x-compressed", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, NBUFFERS);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_raw_single_audio_stream_demuxer_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)raw-audio, " "stream1=(string)none", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->raw == TRUE);
    fail_unless_equals_int (csink->n_raw, NBUFFERS);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_compressed_single_audio_stream_demuxer_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)compressed-audio, " "stream1=(string)none", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, NBUFFERS);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_raw_audio_video_stream_demuxer_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)raw-audio, " "stream1=(string)raw-video", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->raw == TRUE);
    fail_unless_equals_int (csink->n_raw, NBUFFERS);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->raw == TRUE);
    fail_unless_equals_int (csink->n_raw, NBUFFERS);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_compressed_audio_video_stream_demuxer_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)compressed-audio, " "stream1=(string)compressed-video",
      TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, NBUFFERS);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->raw == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, NBUFFERS);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_raw_compressed_video_stream_demuxer_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)raw-video, " "stream1=(string)compressed-video", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless_equals_int (csink->n_raw + csink->n_compressed, NBUFFERS);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_raw_compressed_audio_stream_demuxer_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)raw-audio, " "stream1=(string)compressed-audio", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);
  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless_equals_int (csink->n_raw + csink->n_compressed, NBUFFERS);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

#if 0
typedef struct
{
  GstElement *playbin;
  GstElement *sink;
} SwitchBufferProbeCtx;

static gboolean
switch_video_buffer_probe (GstPad * pad, GstBuffer * buffer,
    SwitchBufferProbeCtx * ctx)
{
  GstElement *playbin = ctx->playbin;
  GstVideoCodecSink *sink = (GstVideoCodecSink *) ctx->sink;

  if (sink->n_raw + sink->n_compressed == NBUFFERS / 2) {
    gint cur_video;

    g_object_get (G_OBJECT (playbin), "current-video", &cur_video, NULL);
    cur_video = (cur_video == 0) ? 1 : 0;
    g_object_set (G_OBJECT (playbin), "current-video", cur_video, NULL);
  }

  return TRUE;
}

GST_START_TEST (test_raw_compressed_video_stream_demuxer_switch_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;
  GstPad *pad;
  SwitchBufferProbeCtx switch_ctx;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)raw-video, " "stream1=(string)compressed-video", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL);
  switch_ctx.playbin = playbin;
  switch_ctx.sink = sink;
  gst_pad_add_buffer_probe (pad, G_CALLBACK (switch_video_buffer_probe),
      &switch_ctx);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->n_raw > 0);
    fail_unless (csink->n_compressed > 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_compressed_raw_video_stream_demuxer_switch_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;
  GstPad *pad;
  SwitchBufferProbeCtx switch_ctx;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)compressed-video, " "stream1=(string)raw-video", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL);
  switch_ctx.playbin = playbin;
  switch_ctx.sink = sink;
  gst_pad_add_buffer_probe (pad, G_CALLBACK (switch_video_buffer_probe),
      &switch_ctx);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->n_raw > 0);
    fail_unless (csink->n_compressed > 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_raw_raw_video_stream_demuxer_switch_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;
  GstPad *pad;
  SwitchBufferProbeCtx switch_ctx;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)raw-video, " "stream1=(string)raw-video", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL);
  switch_ctx.playbin = playbin;
  switch_ctx.sink = sink;
  gst_pad_add_buffer_probe (pad, G_CALLBACK (switch_video_buffer_probe),
      &switch_ctx);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless (csink->n_raw > 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST
    (test_compressed_compressed_video_stream_demuxer_switch_manual_sink) {
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;
  GstPad *pad;
  SwitchBufferProbeCtx switch_ctx;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)compressed-video, " "stream1=(string)compressed-video",
      TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL);
  switch_ctx.playbin = playbin;
  switch_ctx.sink = sink;
  gst_pad_add_buffer_probe (pad, G_CALLBACK (switch_video_buffer_probe),
      &switch_ctx);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless (csink->n_compressed > 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

static gboolean
switch_audio_buffer_probe (GstPad * pad, GstBuffer * buffer,
    SwitchBufferProbeCtx * ctx)
{
  GstElement *playbin = ctx->playbin;
  GstAudioCodecSink *sink = (GstAudioCodecSink *) ctx->sink;

  if (sink->n_raw + sink->n_compressed == NBUFFERS / 3) {
    gint cur_audio;

    g_object_get (G_OBJECT (playbin), "current-audio", &cur_audio, NULL);
    cur_audio = (cur_audio == 0) ? 1 : 0;
    g_object_set (G_OBJECT (playbin), "current-audio", cur_audio, NULL);
  }

  return TRUE;
}

GST_START_TEST (test_raw_compressed_audio_stream_demuxer_switch_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;
  GstPad *pad;
  SwitchBufferProbeCtx switch_ctx;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)raw-audio, " "stream1=(string)compressed-audio", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL);
  switch_ctx.playbin = playbin;
  switch_ctx.sink = sink;
  gst_pad_add_buffer_probe (pad, G_CALLBACK (switch_audio_buffer_probe),
      &switch_ctx);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->n_raw > 0);
    fail_unless (csink->n_compressed > 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_compressed_raw_audio_stream_demuxer_switch_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;
  GstPad *pad;
  SwitchBufferProbeCtx switch_ctx;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)compressed-audio, " "stream1=(string)raw-audio", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL);
  switch_ctx.playbin = playbin;
  switch_ctx.sink = sink;
  gst_pad_add_buffer_probe (pad, G_CALLBACK (switch_audio_buffer_probe),
      &switch_ctx);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->n_raw > 0);
    fail_unless (csink->n_compressed > 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST (test_raw_raw_audio_stream_demuxer_switch_manual_sink)
{
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;
  GstPad *pad;
  SwitchBufferProbeCtx switch_ctx;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)raw-audio, " "stream1=(string)raw-audio", TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL);
  switch_ctx.playbin = playbin;
  switch_ctx.sink = sink;
  gst_pad_add_buffer_probe (pad, G_CALLBACK (switch_audio_buffer_probe),
      &switch_ctx);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless (csink->n_raw > 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;

GST_START_TEST
    (test_compressed_compressed_audio_stream_demuxer_switch_manual_sink) {
  GstMessage *msg;
  GstElement *playbin;
  GstElement *sink;
  GstBus *bus;
  gboolean done = FALSE;
  GstPad *pad;
  SwitchBufferProbeCtx switch_ctx;

  fail_unless (gst_element_register (NULL, "capssrc", GST_RANK_PRIMARY,
          gst_caps_src_get_type ()));
  fail_unless (gst_element_register (NULL, "codecdemuxer",
          GST_RANK_PRIMARY + 100, gst_codec_demuxer_get_type ()));
  fail_unless (gst_element_register (NULL, "audiocodecsink",
          GST_RANK_PRIMARY + 100, gst_audio_codec_sink_get_type ()));
  fail_unless (gst_element_register (NULL, "videocodecsink",
          GST_RANK_PRIMARY + 100, gst_video_codec_sink_get_type ()));

  playbin = create_playbin ("caps:application/x-container, "
      "stream0=(string)compressed-audio, " "stream1=(string)compressed-audio",
      TRUE);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_READY),
      GST_STATE_CHANGE_SUCCESS);

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  pad = gst_element_get_static_pad (sink, "sink");
  fail_unless (pad != NULL);
  switch_ctx.playbin = playbin;
  switch_ctx.sink = sink;
  gst_pad_add_buffer_probe (pad, G_CALLBACK (switch_audio_buffer_probe),
      &switch_ctx);
  gst_object_unref (pad);
  gst_object_unref (sink);

  fail_unless_equals_int (gst_element_set_state (playbin, GST_STATE_PLAYING),
      GST_STATE_CHANGE_ASYNC);

  bus = gst_element_get_bus (playbin);

  while (!done) {
    msg = gst_bus_poll (bus, GST_MESSAGE_ANY, -1);

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        done = TRUE;
        break;
      case GST_MESSAGE_ERROR:
        fail_if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR);
        break;
      default:
        break;
    }
    gst_message_unref (msg);
  }
  gst_object_unref (bus);

  g_object_get (G_OBJECT (playbin), "video-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstVideoCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_video_codec_sink_get_type ());
    csink = (GstVideoCodecSink *) sink;
    fail_unless (csink->audio == FALSE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless_equals_int (csink->n_compressed, 0);
    gst_object_unref (sink);
  }

  g_object_get (G_OBJECT (playbin), "audio-sink", &sink, NULL);
  fail_unless (sink != NULL);
  {
    GstAudioCodecSink *csink;

    fail_unless (G_TYPE_FROM_INSTANCE (sink) ==
        gst_audio_codec_sink_get_type ());
    csink = (GstAudioCodecSink *) sink;
    fail_unless (csink->audio == TRUE);
    fail_unless_equals_int (csink->n_raw, 0);
    fail_unless (csink->n_compressed > 0);
    gst_object_unref (sink);
  }

  gst_element_set_state (playbin, GST_STATE_NULL);
  gst_object_unref (playbin);
}

GST_END_TEST;
#endif
#endif

static Suite *
playbin_compressed_suite (void)
{
  Suite *s = suite_create ("playbin_compressed");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);

#ifndef GST_DISABLE_REGISTRY
  tcase_add_test (tc_chain, test_raw_single_video_stream_manual_sink);
  tcase_add_test (tc_chain, test_raw_single_audio_stream_manual_sink);
  tcase_add_test (tc_chain, test_compressed_single_video_stream_manual_sink);
  tcase_add_test (tc_chain, test_compressed_single_audio_stream_manual_sink);

  tcase_add_test (tc_chain, test_raw_single_video_stream_demuxer_manual_sink);
  tcase_add_test (tc_chain, test_raw_single_audio_stream_demuxer_manual_sink);
  tcase_add_test (tc_chain,
      test_compressed_single_video_stream_demuxer_manual_sink);
  tcase_add_test (tc_chain,
      test_compressed_single_audio_stream_demuxer_manual_sink);

  tcase_add_test (tc_chain, test_raw_audio_video_stream_demuxer_manual_sink);
  tcase_add_test (tc_chain,
      test_compressed_audio_video_stream_demuxer_manual_sink);

  tcase_add_test (tc_chain,
      test_raw_compressed_audio_stream_demuxer_manual_sink);
  tcase_add_test (tc_chain,
      test_raw_compressed_video_stream_demuxer_manual_sink);

  /* These tests need something like the stream-activate event
   * and are racy otherwise */
#if 0
  tcase_add_test (tc_chain,
      test_raw_raw_audio_stream_demuxer_switch_manual_sink);
  tcase_add_test (tc_chain,
      test_raw_compressed_audio_stream_demuxer_switch_manual_sink);
  tcase_add_test (tc_chain,
      test_compressed_raw_audio_stream_demuxer_switch_manual_sink);
  tcase_add_test (tc_chain,
      test_compressed_compressed_audio_stream_demuxer_switch_manual_sink);
  tcase_add_test (tc_chain,
      test_raw_raw_video_stream_demuxer_switch_manual_sink);
  tcase_add_test (tc_chain,
      test_raw_compressed_video_stream_demuxer_switch_manual_sink);
  tcase_add_test (tc_chain,
      test_compressed_raw_video_stream_demuxer_switch_manual_sink);
  tcase_add_test (tc_chain,
      test_compressed_compressed_video_stream_demuxer_switch_manual_sink);
#endif
#endif

  return s;
}

GST_CHECK_MAIN (playbin_compressed);
