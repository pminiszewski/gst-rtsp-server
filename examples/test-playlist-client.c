/* GStreamer
 * Copyright (C) 2008 Wim Taymans <wim.taymans at gmail.com>
 * Copyright (C) 2014 Jan Schmidt <jan@centricular.com>
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

#include <stdlib.h>

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <rpmeta.h>

/* This value is only suitable for local networks with no congestion */
#define LATENCY 40

#define PIPELINE_DESC "rtspsrc name=src ! rpdepay ! rtpgstdepay ! opusdec ! " \
  "audiobuffersplit output-buffer-duration=512/48000 name=split ! autoaudiosink"

#define FRAMES_PER_BLOCK 512

static gboolean
message (GstBus * bus, GstMessage * message, gpointer user_data)
{
  GMainLoop *loop = user_data;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:{
      GError *err = NULL;
      gchar *name, *debug = NULL;

      name = gst_object_get_path_string (message->src);
      gst_message_parse_error (message, &err, &debug);

      g_printerr ("ERROR: from element %s: %s\n", name, err->message);
      if (debug != NULL)
        g_printerr ("Additional debug info:\n%s\n", debug);

      g_error_free (err);
      g_free (debug);
      g_free (name);

      g_main_loop_quit (loop);
      break;
    }
    case GST_MESSAGE_WARNING:{
      GError *err = NULL;
      gchar *name, *debug = NULL;

      name = gst_object_get_path_string (message->src);
      gst_message_parse_warning (message, &err, &debug);

      g_printerr ("ERROR: from element %s: %s\n", name, err->message);
      if (debug != NULL)
        g_printerr ("Additional debug info:\n%s\n", debug);

      g_error_free (err);
      g_free (debug);
      g_free (name);
      break;
    }
    case GST_MESSAGE_EOS:
      g_print ("Got EOS\n");
      g_main_loop_quit (loop);
      break;
    default:
      break;
  }

  return TRUE;
}

typedef struct
{
  guint64 sample_offset;
  gboolean first_buffer;
  gboolean received_samples_event;
  GstAudioInfo ainfo;
} ProbeData;

static gint
compare_meta (RpMeta *rmeta1, RpMeta *rmeta2)
{
  if (rmeta1->offset < rmeta2->offset)
    return -1;
  else if (rmeta1->offset == rmeta2->offset)
    return 0;
  return 1;
}

GstPadProbeReturn
parse_meta_cb (GstPad *pad, GstPadProbeInfo *info, gpointer udata)
{
  ProbeData *pdata = (ProbeData *) udata;

  if (GST_AUDIO_INFO_IS_VALID (&pdata->ainfo) &&
      (info->type & GST_PAD_PROBE_TYPE_BUFFER) &&
      pdata->received_samples_event) {
    GstMeta *meta;
    gpointer state = NULL;
    guint64 samples_in_buffer = gst_util_uint64_scale (gst_buffer_get_size (info->data), pdata->ainfo.channels, pdata->ainfo.bpf);
    guint64 buffer_start = pdata->sample_offset;
    guint64 buffer_end = buffer_start + samples_in_buffer;
    GList *sorted = NULL;
    GList *tmp;

    pdata->first_buffer = TRUE;

    /* First sort the meta */
    while ((meta = gst_buffer_iterate_meta (info->data, &state)))
      if (meta->info == RP_META_INFO)
        sorted = g_list_insert_sorted (sorted, meta, (GCompareFunc) compare_meta);

    g_print ("New buffer made up of %" G_GUINT64_FORMAT " samples:\n", samples_in_buffer);

    for (tmp = sorted; tmp; tmp = tmp->next) {
      RpMeta *rmeta = (RpMeta *) tmp->data;
      guint64 block_start = rmeta->offset;
      guint64 block_end = block_start + FRAMES_PER_BLOCK * pdata->ainfo.channels;
      guint64 overlap_start = MAX (buffer_start, block_start);
      guint64 overlap_end = MIN (buffer_end, block_end);

      if (overlap_start < overlap_end)
        g_print ("\t%" G_GUINT64_FORMAT " samples with data %02x\n",
            overlap_end - overlap_start, rmeta->data);
    }
    g_list_free (sorted);
    pdata->sample_offset += samples_in_buffer;
  } else if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    if (GST_EVENT_TYPE (info->data) == GST_EVENT_CAPS) {
      GstCaps *caps;

      gst_event_parse_caps (info->data, &caps);
      gst_audio_info_from_caps (&pdata->ainfo, caps);
      gst_caps_unref (caps);
    } else if (!pdata->first_buffer && GST_EVENT_TYPE (info->data) == GST_EVENT_CUSTOM_DOWNSTREAM) {
      const GstStructure *s = gst_event_get_structure (info->data);

      if (gst_structure_has_name (s, "GstAudioEncoderSamples") &&
          GST_AUDIO_INFO_IS_VALID (&pdata->ainfo)) {
        gst_structure_get_uint64 (s, "processed", &pdata->sample_offset);
        pdata->sample_offset *= pdata->ainfo.channels;
        GST_ERROR ("Got it though, %lu", pdata->sample_offset);
        pdata->received_samples_event = TRUE;
      }
    }
  }

  return GST_PAD_PROBE_OK;
}

int
main (int argc, char *argv[])
{
  GstElement *pipe, *src, *split;
  GMainLoop *loop;
  GError *error = NULL;
  GstPad *split_srcpad;
  ProbeData pdata = {0};

  gst_init (&argc, &argv);

  if (argc < 2) {
    g_print ("usage: %s rtsp://URI\n"
        "example: %s rtsp://localhost:8554/test\n",
        argv[0], argv[0]);
    return -1;
  }

  loop = g_main_loop_new (NULL, FALSE);

  pipe = gst_parse_launch (PIPELINE_DESC, &error);
  g_assert (!error);
  src = gst_bin_get_by_name (GST_BIN (pipe), "src");
  g_object_set (src, "latency", LATENCY, "location", argv[1], NULL);
  gst_object_unref (src);

  split = gst_bin_get_by_name (GST_BIN (pipe), "split");
  g_assert (split);
  split_srcpad = gst_element_get_static_pad (split, "src");
  g_assert (split_srcpad);
  gst_pad_add_probe (split_srcpad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, parse_meta_cb, &pdata, NULL);
  gst_object_unref (split);
  gst_object_unref (split_srcpad);

  if (gst_element_set_state (pipe,
          GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_print ("Failed to set state to PLAYING\n");
    goto exit;
  };

  gst_bus_add_signal_watch (GST_ELEMENT_BUS (pipe));
  g_signal_connect (GST_ELEMENT_BUS (pipe), "message", G_CALLBACK (message),
      loop);

  g_main_loop_run (loop);

exit:
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);
  g_main_loop_unref (loop);

  return 0;
}
