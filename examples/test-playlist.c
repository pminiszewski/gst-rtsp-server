/* GStreamer
 * Copyright (C) 2017 Mathieu Duponchelle <mathieu@centricular.com>
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include <sys/types.h>
#include <sys/stat.h>
/* For g_stat () */
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

static gchar *folder = NULL;

#define CLIP_DESC \
"uridecodebin uri=%s expose-all-streams=false caps=audio/x-raw name=d interleave name=i " \
"d.src_0 ! queue ! audioconvert ! deinterleave name=s " \
"s.src_0 ! queue ! audioconvert ! audioresample ! audio/x-raw,channels=1 ! i.sink_0 " \
"s.src_1 ! queue ! audioconvert ! audioresample ! audio/x-raw,channels=1 ! i.sink_1 " \
"d.src_1 ! queue ! audioconvert ! audioresample ! audio/x-raw,channels=1 ! i.sink_2 " \
"d.src_2 ! queue ! audioconvert ! audioresample ! audio/x-raw,channels=1 ! i.sink_3 " \
"d.src_3 ! queue ! audioconvert ! audioresample ! audio/x-raw,channels=1 ! i.sink_4 " \
"d.src_4 ! queue ! audioconvert ! audioresample ! audio/x-raw,channels=1 ! i.sink_5 " \
"i.src ! capssetter caps=\"audio/x-raw, channels=6, channel-mask=(bitmask)0x3f\" ! audioconvert ! audioresample ! audio/x-raw, rate=48000, format=S16LE ! audioconvert ! audioresample ! " \
"capssetter caps=\"audio/x-raw,channels=6,channel-mask=(bitmask)0x0,layout=interleaved,format=S16LE,rate=48000\""

#define ENCODER "opusenc bitrate=192000"

#define PARSER "opusparse"

#define PAYLOADER "rtpgstpay"

#define OUTPUT_CAPS "audio/x-raw,channels=6,channel-mask=(bitmask)0x0,layout=interleaved,format=S16LE,rate=48000"

/* Audio clip */

#define TEST_TYPE_CLIP (test_clip_get_type ())
G_DECLARE_FINAL_TYPE (TestClip, test_clip, TEST, CLIP, GstBin);

enum
{
  PROP_CLIP_0,
  PROP_CLIP_URI
};

enum
{
  CLIP_DONE,
  LAST_CLIP_SIGNAL,
};

static guint test_clip_signals[LAST_CLIP_SIGNAL] = { 0 };

struct _TestClip
{
  GstBin parent;
  gchar *uri;
  GstPad *srcpad;
  gint emit_done;
};

G_DEFINE_TYPE (TestClip, test_clip, GST_TYPE_BIN);

static void
test_clip_emit_done (TestClip * self, gboolean stopped)
{
  if (g_atomic_int_dec_and_test (&self->emit_done))
    g_signal_emit (self, test_clip_signals[CLIP_DONE], 0, stopped);
}

static void
handle_message (GstBin * bin, GstMessage * message)
{
  TestClip *self = TEST_CLIP (bin);

  switch (GST_MESSAGE_TYPE (message)) {
      /* We intercept and drop errors at the clip level, sequencer should
       * keep going on */
    case GST_MESSAGE_ERROR:
    {
      GError *error = NULL;
      gchar *dbg;

      gst_message_parse_error (message, &error, &dbg);
      GST_DEBUG_OBJECT (self, "Error: %s (%s)", error->message, dbg);

      g_error_free (error);
      g_free (dbg);

      test_clip_emit_done (self, FALSE);

      gst_message_unref (message);
      message = NULL;
    }
    default:
      break;
  }

  if (message)
    GST_BIN_CLASS (test_clip_parent_class)->handle_message (bin, message);
}

static GstPadProbeReturn
event_probe_cb (GstPad * pad, GstPadProbeInfo * info, TestClip * self)
{
  GstPadProbeReturn ret = GST_PAD_PROBE_OK;

  switch (GST_EVENT_TYPE (info->data)) {
    case GST_EVENT_EOS:
    {
      test_clip_emit_done (self, FALSE);
      ret = GST_PAD_PROBE_DROP;
      break;
    }
    default:
      break;
  }

  return ret;
}

static void
test_clip_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec)
{
  TestClip *self = TEST_CLIP (object);

  switch (propid) {
    case PROP_CLIP_URI:
      g_free (self->uri);
      self->uri = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static void
test_clip_finalize (GObject * object)
{
  TestClip *self = TEST_CLIP (object);

  g_free (self->uri);
  gst_object_unref (self->srcpad);
}

static void
test_clip_constructed (GObject * object)
{
  TestClip *self = TEST_CLIP (object);
  GstElement *decodebin;
  gchar *bin_desc;
  GError *error = NULL;
  GstPad *decodebin_srcpad;

  g_assert (self->uri);

  bin_desc = g_strdup_printf (CLIP_DESC, self->uri);
  decodebin = gst_parse_bin_from_description (bin_desc, FALSE, &error);
  g_free (bin_desc);

  g_assert (!error);

  gst_bin_add (GST_BIN (self), decodebin);

  self->srcpad =
      gst_object_ref_sink (gst_ghost_pad_new_no_target ("src", GST_PAD_SRC));
  gst_pad_set_active (self->srcpad, TRUE);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

  decodebin_srcpad = gst_bin_find_unlinked_pad (GST_BIN (decodebin), GST_PAD_SRC);
  g_assert (decodebin_srcpad);
  gst_ghost_pad_set_target (GST_GHOST_PAD (self->srcpad), decodebin_srcpad);
  gst_object_unref (decodebin_srcpad);

  gst_pad_add_probe (self->srcpad,
      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      (GstPadProbeCallback) event_probe_cb, self, NULL);

  gst_bin_sync_children_states (GST_BIN (self));
}

static void
test_clip_class_init (TestClipClass * test_klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (test_klass);
  GstBinClass *gstbin_class = GST_BIN_CLASS (test_klass);

  gobject_class->set_property = test_clip_set_property;
  g_object_class_install_property (gobject_class, PROP_CLIP_URI,
      g_param_spec_string ("uri", "URI",
          "URI of the clip to play back", NULL,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
  gobject_class->finalize = test_clip_finalize;
  gobject_class->constructed = test_clip_constructed;

  test_clip_signals[CLIP_DONE] =
      g_signal_new ("done", G_TYPE_FROM_CLASS (test_klass),
      G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

  gstbin_class->handle_message = handle_message;
}

static void
test_clip_init (TestClip * self)
{
  g_atomic_int_set (&self->emit_done, 1);
}

static void
test_clip_stop (TestClip * self)
{
  test_clip_emit_done (self, TRUE);
}

/* Sequencer of audio clips */

#define TEST_TYPE_SEQUENCER (test_sequencer_get_type ())
G_DECLARE_FINAL_TYPE (TestSequencer, test_sequencer, TEST, SEQUENCER, GstBin);

enum
{
  PROP_SEQUENCER_0,
  PROP_SEQUENCER_FOLDER
};

struct _TestSequencer
{
  GstBin parent;
  gchar *folder;
  GstElement *concat;
  GstElement *payloader;
  GstElement *mixer;
  GstPad *songs_pad;
  GList *uris;
  GList *next_uri;
  gulong sound_probeid;
  GstPad *concat_srcpad;
  gint64 last_mix_pos;
  GQueue *pads_to_release;
  GQueue *clips_to_remove;
  TestClip *current_clip;
};

G_DEFINE_TYPE (TestSequencer, test_sequencer, GST_TYPE_BIN);

static void queue_uri (TestSequencer * self);

static void
test_sequencer_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec)
{
  TestSequencer *self = TEST_SEQUENCER (object);

  switch (propid) {
    case PROP_SEQUENCER_FOLDER:
      g_free (self->folder);
      self->folder = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static void
test_sequencer_finalize (GObject * object)
{
  TestSequencer *self = TEST_SEQUENCER (object);

  g_free (self->folder);
  g_list_free_full (self->uris, g_free);
  g_queue_free (self->pads_to_release);
  g_queue_free (self->clips_to_remove);
}

static gboolean
test_sequencer_list_uris (TestSequencer * self)
{
  gboolean ret = FALSE;
  GDir *dir;
  const gchar *dirent;
  gchar *filename;

  if (!self->folder) {
    g_critical ("Sequencer needs a folder to operate on");
    goto done;
  }

  dir = g_dir_open (self->folder, 0, NULL);
  if (!dir) {
    g_critical ("Sequencer needs a valid folder to operate on");
    goto done;
  }

  while ((dirent = g_dir_read_name (dir))) {
    GStatBuf file_status;

    filename = g_build_filename (self->folder, dirent, NULL);
    if (g_stat (filename, &file_status) < 0) {
      g_free (filename);
      continue;
    }

    if ((file_status.st_mode & S_IFREG)) {
      gchar *uri;

      uri = gst_filename_to_uri (filename, NULL);
      if (uri) {
        self->uris = g_list_prepend (self->uris, uri);
      }
    }
    g_free (filename);
  }

  g_dir_close (dir);

  if (!self->uris) {
    g_critical ("Sequencer needs a non-empty folder to operate on");
    goto done;
  }

  self->uris = g_list_sort (self->uris, (GCompareFunc) g_strcmp0);

  ret = TRUE;

done:
  return ret;
}

static gboolean
remove_clip (TestClip * clip)
{
  TestSequencer *self;
  GstStateChangeReturn res;

  res = gst_element_set_state (GST_ELEMENT (clip), GST_STATE_NULL);

  /* Will eventually work, let's not dispose just yet */
  if (res == GST_STATE_CHANGE_FAILURE)
    return G_SOURCE_CONTINUE;

  self = TEST_SEQUENCER (gst_element_get_parent (clip));

  gst_bin_remove (GST_BIN (self), GST_ELEMENT (clip));
  gst_object_unref (self);

  return G_SOURCE_REMOVE;
}

static void
test_sequencer_next_uri (TestSequencer * self)
{
  self->next_uri = self->next_uri->next;

  if (!self->next_uri)
    self->next_uri = self->uris;
}

static void
test_sequencer_previous_uri (TestSequencer * self)
{
  self->next_uri = self->next_uri->prev;

  if (!self->next_uri)
    self->next_uri = g_list_last (self->uris);
}

static void
clip_done_cb (TestClip * clip, gboolean stopped, TestSequencer * self)
{
  GstPad *clip_srcpad = gst_element_get_static_pad (GST_ELEMENT (clip), "src");
  GstPad *peer = gst_pad_get_peer (clip_srcpad);

  if (!stopped)
    test_sequencer_next_uri (self);

  queue_uri (self);

  gst_pad_unlink (clip_srcpad, peer);
  gst_object_unref (clip_srcpad);

  if (!self->sound_probeid) {
    gst_element_release_request_pad (self->concat, peer);
    gst_object_unref (peer);
    g_idle_add ((GSourceFunc) remove_clip, clip);
  } else {
    g_queue_push_tail (self->pads_to_release, peer);
    g_queue_push_tail (self->clips_to_remove, clip);
  }

}

static void
queue_uri (TestSequencer * self)
{
  gchar *uri;
  GstPad *concat_sinkpad, *clip_srcpad;

  uri = self->next_uri->data;

  GST_INFO_OBJECT (self, "Queuing %s", uri);

  self->current_clip = g_object_new (TEST_TYPE_CLIP, "uri", uri, NULL);
  gst_bin_add (GST_BIN (self), GST_ELEMENT (self->current_clip));

  clip_srcpad = gst_element_get_static_pad (GST_ELEMENT (self->current_clip), "src");
  concat_sinkpad = gst_element_get_request_pad (self->concat, "sink_%u");
  gst_pad_link (clip_srcpad, concat_sinkpad);
  gst_object_unref (concat_sinkpad);
  gst_object_unref (clip_srcpad);

  g_signal_connect (self->current_clip, "done", G_CALLBACK (clip_done_cb), self);

  gst_element_sync_state_with_parent (GST_ELEMENT (self->current_clip));
}

static gboolean
test_sequencer_start (TestSequencer * self)
{
  gboolean ret = FALSE;
  if (!test_sequencer_list_uris (self))
    goto done;

  self->next_uri = self->uris;

  queue_uri (self);

  ret = TRUE;

done:
  return ret;
}

static GstStateChangeReturn
test_sequencer_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!test_sequencer_start (TEST_SEQUENCER (element))) {
        ret = GST_STATE_CHANGE_FAILURE;
        goto done;
      }
      break;
    default:
      break;
  }

  ret =
      GST_ELEMENT_CLASS (test_sequencer_parent_class)->change_state (element,
      transition);

done:
  return ret;
}

static void
test_sequencer_constructed (GObject * object)
{
  TestSequencer *self = TEST_SEQUENCER (object);
  GstElement *enc, *parse, *src, *conv, *resample, *capsfilter;
  GstPad *mixer_sinkpad, *resample_srcpad;
  GstCaps *output_caps;
  GError *error = NULL;

  src = gst_element_factory_make ("audiotestsrc", NULL);
  g_object_set (src, "is-live", TRUE, "volume", 0, NULL);
  gst_bin_add (GST_BIN (self), src);

  conv = gst_element_factory_make ("audioconvert", NULL);
  gst_bin_add (GST_BIN (self), conv);

  resample = gst_element_factory_make ("audioresample", NULL);
  gst_bin_add (GST_BIN (self), resample);

  self->mixer = gst_element_factory_make ("audiomixer", NULL);
  gst_bin_add (GST_BIN (self), self->mixer);

  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  gst_bin_add (GST_BIN (self), capsfilter);
  output_caps = gst_caps_from_string (OUTPUT_CAPS);
  g_object_set (capsfilter, "caps", output_caps, NULL);
  gst_caps_unref (output_caps);

  self->concat = gst_element_factory_make ("concat", NULL);
  gst_bin_add (GST_BIN (self), self->concat);

  enc = gst_parse_bin_from_description_full (ENCODER, FALSE, NULL,
      GST_PARSE_FLAG_NO_SINGLE_ELEMENT_BINS, &error);
  g_assert_no_error (error);
  g_assert_false (GST_IS_BIN (enc));
  gst_bin_add (GST_BIN (self), enc);

  parse = gst_parse_bin_from_description_full (PARSER, FALSE, NULL,
      GST_PARSE_FLAG_NO_SINGLE_ELEMENT_BINS, &error);
  g_assert_no_error (error);
  g_assert_false (GST_IS_BIN (parse));
  gst_bin_add (GST_BIN (self), parse);

  g_assert (gst_element_link_many (src, conv, resample, NULL));
  g_assert (gst_element_link_many (enc, parse, self->payloader, NULL));

  resample_srcpad = gst_element_get_static_pad (resample, "src");
  mixer_sinkpad = gst_element_get_request_pad (self->mixer, "sink_%u");
  gst_pad_link (resample_srcpad, mixer_sinkpad);
  gst_object_unref (resample_srcpad);
  gst_object_unref (mixer_sinkpad);

  self->concat_srcpad = gst_element_get_static_pad (self->concat, "src");
  self->songs_pad = gst_element_get_request_pad (self->mixer, "sink_%u");
  gst_pad_link (self->concat_srcpad, self->songs_pad);
  gst_object_unref (self->concat_srcpad);
  gst_object_unref (self->songs_pad);

  gst_element_link_many (self->mixer, capsfilter, enc, NULL);

  gst_bin_sync_children_states (GST_BIN (self));

}

static void
test_sequencer_class_init (TestSequencerClass * test_klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (test_klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (test_klass);

  gobject_class->set_property = test_sequencer_set_property;
  g_object_class_install_property (gobject_class, PROP_SEQUENCER_FOLDER,
      g_param_spec_string ("folder", "Folder",
          "Path to the songs folder", NULL,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
  gobject_class->finalize = test_sequencer_finalize;
  gobject_class->constructed = test_sequencer_constructed;
  gstelement_class->change_state = test_sequencer_change_state;
}

static void
test_sequencer_init (TestSequencer * self)
{
  GError *error = NULL;

  /* RtspMedia looks for an element named pay0, a bit clunky but it works */
  self->payloader = gst_parse_bin_from_description_full (PAYLOADER, FALSE,
      NULL, GST_PARSE_FLAG_NO_SINGLE_ELEMENT_BINS, &error);
  g_assert_no_error (error);
  g_assert_false (GST_IS_BIN (self->payloader));
  gst_element_set_name (self->payloader, "pay0");
  self->pads_to_release = g_queue_new();
  self->clips_to_remove = g_queue_new();
  gst_bin_add (GST_BIN (self), self->payloader);
}

static void
print_current (TestSequencer * self)
{
  gchar *unescaped;

  unescaped = g_uri_unescape_string ((gchar *) self->next_uri->data, NULL);
  g_print ("Will play back %s\n", unescaped);
  g_free (unescaped);
}

static void
test_sequencer_previous (TestSequencer * self)
{
  test_sequencer_previous_uri (self);
  print_current (self);
  test_clip_stop (self->current_clip);
}

static void
test_sequencer_next (TestSequencer * self)
{
  GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (self), GST_DEBUG_GRAPH_SHOW_ALL, "next");
  test_sequencer_next_uri (self);
  print_current (self);
  test_clip_stop (self->current_clip);
}

static GstPadProbeReturn
pad_blocked_cb (GstPad * pad, GstPadProbeInfo * info, gpointer udata)
{
  return GST_PAD_PROBE_OK;
}

static void
test_sequencer_pause (TestSequencer * self)
{
  if (self->sound_probeid) {
    g_print ("Already paused\n");
    return;
  }

  g_print ("Pausing\n");

  self->sound_probeid =
      gst_pad_add_probe (self->concat_srcpad,
      GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, pad_blocked_cb, self, NULL);
  gst_element_query_position (self->mixer, GST_FORMAT_TIME,
      &self->last_mix_pos);
}

static void
test_sequencer_play (TestSequencer * self)
{
  gint64 mix_pos;

  if (!self->sound_probeid) {
    g_print ("Already playing\n");
    return;
  }

  g_print ("Resuming playback\n");
  gst_element_query_position (self->mixer, GST_FORMAT_TIME, &mix_pos);
  GstEvent *segment_event =
      gst_pad_get_sticky_event (self->songs_pad, GST_EVENT_SEGMENT, 0);
  gst_pad_set_offset (self->songs_pad, mix_pos - self->last_mix_pos);
  gst_pad_send_event (self->songs_pad, segment_event);
  gst_pad_remove_probe (self->concat_srcpad, self->sound_probeid);
  self->sound_probeid = 0;

  while (!g_queue_is_empty (self->pads_to_release)) {
    GstPad *pad = g_queue_pop_head (self->pads_to_release);
    gst_element_release_request_pad (self->concat, pad);
  }
  while (!g_queue_is_empty (self->pads_to_release)) {
    TestClip *clip = g_queue_pop_head (self->clips_to_remove);
    g_idle_add ((GSourceFunc) remove_clip, clip);
  }
}

/* Custom RTSPMediaFactory subclass */

#define TEST_TYPE_RTSP_MEDIA_FACTORY      (test_rtsp_media_factory_get_type ())
G_DECLARE_FINAL_TYPE (TestRTSPMediaFactory, test_rtsp_media_factory, TEST,
    RTSP_MEDIA_FACTORY, GstRTSPMediaFactory);

struct _TestRTSPMediaFactory
{
  GstRTSPMediaFactory parent;
};

G_DEFINE_TYPE (TestRTSPMediaFactory, test_rtsp_media_factory,
    GST_TYPE_RTSP_MEDIA_FACTORY);

static gboolean pause_cb (TestSequencer * seq);

static gboolean
play_cb (TestSequencer * seq)
{
  test_sequencer_play (seq);
  g_timeout_add_seconds (5, (GSourceFunc) pause_cb, seq);
  return G_SOURCE_REMOVE;
}

static gboolean
pause_cb (TestSequencer * seq)
{
  test_sequencer_pause (seq);
  g_timeout_add_seconds (5, (GSourceFunc) play_cb, seq);
  return G_SOURCE_REMOVE;
}

static gboolean
io_callback (GIOChannel * io, GIOCondition condition, gpointer udata)
{
  TestSequencer *sequencer = TEST_SEQUENCER (udata);
  GError *error = NULL;
  gchar *line;
  gsize size;

  switch (g_io_channel_read_line (io, &line, &size, NULL, &error)) {
    case G_IO_STATUS_NORMAL:
      if (!g_strcmp0 (line, "help\n")) {
        g_print ("next: play next song\n");
        g_print ("prev: play previous song\n");
        g_print ("pause: stop playback\n");
        g_print ("play: resume playback\n");
      } else if (!g_strcmp0 (line, "next\n")) {
        test_sequencer_next (sequencer);
      } else if (!g_strcmp0 (line, "prev\n")) {
        test_sequencer_previous (sequencer);
      } else if (!g_strcmp0 (line, "pause\n")) {
        test_sequencer_pause (sequencer);
      } else if (!g_strcmp0 (line, "play\n")) {
        test_sequencer_play (sequencer);
      } else if (g_strcmp0 (line, "\n")) {
        g_print ("Unknown command, type help to list available commands\n");
      }
      g_free (line);
      g_print ("$ ");
      return TRUE;
    case G_IO_STATUS_ERROR:
      g_error_free (error);
      return FALSE;
    case G_IO_STATUS_EOF:
      return TRUE;
    case G_IO_STATUS_AGAIN:
      return TRUE;
    default:
      g_return_val_if_reached (FALSE);
      break;
  }

  return FALSE;
}

static GstElement *
create_element (GstRTSPMediaFactory * factory, const GstRTSPUrl * url)
{
  GstElement *res;

  res = g_object_new (TEST_TYPE_SEQUENCER, "folder", folder, NULL);

  return GST_ELEMENT (res);
}

static void
test_rtsp_media_factory_class_init (TestRTSPMediaFactoryClass * test_klass)
{
  GstRTSPMediaFactoryClass *klass = (GstRTSPMediaFactoryClass *) (test_klass);
  klass->create_element = create_element;
}

static void
test_rtsp_media_factory_init (TestRTSPMediaFactory * self)
{
}

static void
media_unprepared_cb (GstRTSPMedia *media, gpointer udata)
{
  guint watch_id = GPOINTER_TO_UINT (udata);
  g_source_remove (watch_id);
  g_print ("Session closed\n");
}

static void
media_prepared_cb (GstRTSPMedia *media, gpointer udata)
{
  TestSequencer *seq = TEST_SEQUENCER (gst_rtsp_media_get_element (media));
  GIOChannel *io;
  guint watch_id;

  /* Read stdin */
#ifdef G_OS_WIN32
  io = g_io_channel_win32_new_fd (_fileno (stdin));
#else
  io = g_io_channel_unix_new (STDIN_FILENO);
#endif

  watch_id = g_io_add_watch (io, G_IO_IN, io_callback, seq);
  g_io_channel_unref (io);
  gst_object_unref (seq);

  g_print ("Session opened, type help to list commands\n");
  g_print ("$ ");

  g_signal_connect (media, "unprepared", G_CALLBACK (media_unprepared_cb), GUINT_TO_POINTER (watch_id));
}

static void
media_constructed_cb (GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer udata)
{
  g_signal_connect (media, "prepared", G_CALLBACK (media_prepared_cb), NULL);
}

static gboolean
check_elements_exist (const gchar * name, ...)
{
  va_list varargs;
  gboolean res = TRUE;

  va_start (varargs, name);

  while (name && res) {
    GstElementFactory *fac = gst_element_factory_find (name);

    if (!fac) {
      g_print ("Element should exist: %s\n", name);
      res = FALSE;
    } else {
      g_object_unref (fac);
    }

    name = va_arg (varargs, gchar *);
  }

  va_end (varargs);
  return res;
}


static gboolean
sanity_check (void)
{
  gboolean ret = TRUE;

  if (!check_elements_exist ("audiotestsrc", "audioconvert",
          "audioresample", "audiomixer", "concat", NULL)) {
    g_print ("Sanity checks failed\n");
    ret = FALSE;
    goto done;
  }

done:
  return ret;
}

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  GstRTSPServer *server;
  GstRTSPMountPoints *mounts;
  GstRTSPMediaFactory *factory;

  gst_init (&argc, &argv);

  if (argc < 2) {
    g_print ("usage: %s <audio folder> \n"
        "example: %s $HOME/Music\n", argv[0], argv[0]);
    return -1;
  }

  if (!sanity_check ())
    return -1;

  folder = g_strdup (argv[1]);

  loop = g_main_loop_new (NULL, FALSE);

  server = gst_rtsp_server_new ();
  mounts = gst_rtsp_server_get_mount_points (server);
  factory = g_object_new (TEST_TYPE_RTSP_MEDIA_FACTORY, NULL);
  gst_rtsp_media_factory_set_shared (factory, TRUE);
  g_signal_connect (factory, "media-constructed", G_CALLBACK (media_constructed_cb), NULL);
  gst_rtsp_mount_points_add_factory (mounts, "/test", factory);
  g_object_unref (mounts);
  gst_rtsp_server_attach (server, NULL);

  /* start serving */
  g_print ("ready to serve at rtsp://127.0.0.1:8554/test\n");
  g_main_loop_run (loop);

  g_free (folder);

  return 0;
}
