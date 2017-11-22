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
#include <gst/sdp/gstsdpmessage.h>
#include <gio/gio.h>
#include <string.h>
#ifdef G_OS_WIN32
#include <Winsock2.h>
#else
#include <sys/socket.h>
#endif
#include <rpmeta.h>

/* This value is only suitable for local networks with no congestion */
#define LATENCY 40

#define PIPELINE_DESC "rtspsrc name=src ! rpdepay ! rtpgstdepay ! opusdec ! " \
  "audiobuffersplit output-buffer-duration=512/48000 name=split ! autoaudiosink"

#define FRAMES_PER_BLOCK 512

#define DEFAULT_SAP_ADDRESS "224.0.0.56"
#define DEFAULT_SAP_PORT 9875
#define MIME_TYPE "application/sdp"
#define SDP_HEADER "v=0\n"

typedef struct
{
  GSocket *sock;
  gchar *session_name;
  GstElement *pipe;
  guint64 sample_offset;
  gboolean first_buffer;
  gboolean received_samples_event;
  GstAudioInfo ainfo;
  guint watch_id;
} Context;


static gboolean
message (GstBus * bus, GstMessage * message, gpointer user_data)
{
  Context *ctx = (Context *) user_data;

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

      gst_bus_remove_signal_watch (GST_ELEMENT_BUS (ctx->pipe));
      gst_element_set_state (ctx->pipe, GST_STATE_NULL);
      gst_object_unref (ctx->pipe);
      ctx->received_samples_event = FALSE;
      gst_audio_info_init (&ctx->ainfo);
      ctx->first_buffer = FALSE;
      ctx->pipe = NULL;
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
    default:
      break;
  }

  return TRUE;
}

static gint
compare_meta (RpMeta * rmeta1, RpMeta * rmeta2)
{
  if (rmeta1->offset < rmeta2->offset)
    return -1;
  else if (rmeta1->offset == rmeta2->offset)
    return 0;
  return 1;
}

GstPadProbeReturn
parse_meta_cb (GstPad * pad, GstPadProbeInfo * info, gpointer udata)
{
  Context *ctx = (Context *) udata;

  GST_DEBUG ("parsing meta, received %" GST_PTR_FORMAT, info->data);

  if (GST_AUDIO_INFO_IS_VALID (&ctx->ainfo) &&
      (info->type & GST_PAD_PROBE_TYPE_BUFFER) && ctx->received_samples_event) {
    GstMeta *meta;
    gpointer state = NULL;
    guint64 samples_in_buffer =
        gst_util_uint64_scale (gst_buffer_get_size (info->data),
        ctx->ainfo.channels, ctx->ainfo.bpf);
    guint64 buffer_start = ctx->sample_offset;
    guint64 buffer_end = buffer_start + samples_in_buffer;
    GList *sorted = NULL;
    GList *tmp;

    ctx->first_buffer = TRUE;

    /* First sort the meta */
    while ((meta = gst_buffer_iterate_meta (info->data, &state)))
      if (meta->info == RP_META_INFO)
        sorted =
            g_list_insert_sorted (sorted, meta, (GCompareFunc) compare_meta);

    g_print ("New buffer made up of %" G_GUINT64_FORMAT " samples:\n",
        samples_in_buffer);

    for (tmp = sorted; tmp; tmp = tmp->next) {
      RpMeta *rmeta = (RpMeta *) tmp->data;
      guint64 block_start = rmeta->offset;
      guint64 block_end = block_start + FRAMES_PER_BLOCK * ctx->ainfo.channels;
      guint64 overlap_start = MAX (buffer_start, block_start);
      guint64 overlap_end = MIN (buffer_end, block_end);

      GST_DEBUG ("parsing meta with offset %" G_GUINT64_FORMAT, rmeta->offset);

      if (overlap_start < overlap_end)
        g_print ("\t%" G_GUINT64_FORMAT " samples with data %02x\n",
            overlap_end - overlap_start, rmeta->data);
    }
    g_list_free (sorted);
    ctx->sample_offset += samples_in_buffer;
  } else if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    if (GST_EVENT_TYPE (info->data) == GST_EVENT_CAPS) {
      GstCaps *caps;

      gst_event_parse_caps (info->data, &caps);
      gst_audio_info_from_caps (&ctx->ainfo, caps);
    } else if (!ctx->first_buffer
        && GST_EVENT_TYPE (info->data) == GST_EVENT_CUSTOM_DOWNSTREAM) {
      const GstStructure *s = gst_event_get_structure (info->data);

      if (gst_structure_has_name (s, "GstAudioEncoderSamples") &&
          GST_AUDIO_INFO_IS_VALID (&ctx->ainfo)) {
        gst_structure_get_uint64 (s, "processed", &ctx->sample_offset);
        ctx->sample_offset *= ctx->ainfo.channels;
        ctx->received_samples_event = TRUE;
      }
    }
  }

  return GST_PAD_PROBE_OK;
}

static int
recv_sap (Context * ctx, gchar ** sdp, gboolean * goodbye)
{
  GInputVector iovec;
  gssize len;
  guint32 header;
  guint six, ac, k;
  gchar *e;

  iovec.buffer = g_malloc (4096 * sizeof (gchar));
  iovec.size = 4096;
  len =
      g_socket_receive_message (ctx->sock, NULL, &iovec, 1, NULL, NULL, NULL,
      NULL, NULL);

  if (len < 0)
    goto done;

  if (len < 4)
    goto fail;

  memcpy (&header, iovec.buffer, sizeof (guint32));
  header = g_ntohl (header);

  if (header >> 29 != 1)
    goto fail;

  if ((header >> 25) & 1)
    goto fail;

  if ((header >> 24) & 1)
    goto fail;

  six = (header >> 28) & 1U;
  ac = (header >> 16) & 0xFFU;

  k = 4 + (six ? 16U : 4U) + ac * 4U;
  if ((guint) len < k)
    goto fail;

  e = (guint8 *) iovec.buffer + k;
  len -= (gint) k;

  if ((guint) len >= sizeof (MIME_TYPE) && !strcmp (e, MIME_TYPE)) {
    e += sizeof (MIME_TYPE);
    len -= (gint) sizeof (MIME_TYPE);
  } else if ((guint) len < sizeof (SDP_HEADER) - 1
      || strncmp (e, SDP_HEADER, sizeof (SDP_HEADER) - 1)) {
    goto fail;
  }

  *sdp = g_strndup (e, (guint) len);

  *goodbye = ! !((header >> 26) & 1);

done:
  g_free (iovec.buffer);
  return len;

fail:
  len = -1;
  goto done;
}

static gboolean
start_pipeline (Context * ctx, const gchar * location)
{
  GError *err = NULL;
  GstPad *split_srcpad;
  GstElement *src, *split;
  gboolean ret = FALSE;

  ctx->pipe = gst_parse_launch (PIPELINE_DESC, &err);
  g_assert (!err);
  src = gst_bin_get_by_name (GST_BIN (ctx->pipe), "src");
  g_object_set (src, "latency", LATENCY, "location", location, NULL);
  gst_object_unref (src);

  split = gst_bin_get_by_name (GST_BIN (ctx->pipe), "split");
  g_assert (split);
  split_srcpad = gst_element_get_static_pad (split, "src");
  g_assert (split_srcpad);
  gst_pad_add_probe (split_srcpad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM,
      parse_meta_cb, ctx, NULL);
  gst_object_unref (split);
  gst_object_unref (split_srcpad);

  if (gst_element_set_state (ctx->pipe,
          GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    g_print ("Failed to set state to PLAYING\n");
    goto done;
  };

  gst_bus_add_signal_watch (GST_ELEMENT_BUS (ctx->pipe));
  g_signal_connect (GST_ELEMENT_BUS (ctx->pipe), "message",
      G_CALLBACK (message), ctx);

  ret = TRUE;

done:
  return ret;
}

static gboolean
sap_cb (GIOChannel * io, GIOCondition condition, gpointer udata)
{
  gboolean goodbye = FALSE;
  Context *ctx = (Context *) udata;
  gchar *sdp = NULL;
  GstSDPMessage msg = { 0, };
  gboolean ret = FALSE;
  const gchar *session_name;
  const gchar *uri;

  if (ctx->pipe)
    return TRUE;

  if (recv_sap (ctx, &sdp, &goodbye) < 0)
    goto done;

  ret = TRUE;

  if (gst_sdp_message_parse_buffer ((guint8 *) sdp, strlen (sdp),
          &msg) != GST_SDP_OK) {
    goto done;
  }

  session_name = gst_sdp_message_get_session_name (&msg);

  if (!g_strcmp0 (session_name, ctx->session_name)) {
    uri = gst_sdp_message_get_uri (&msg);
    if (uri)
      start_pipeline (ctx, uri);
  }

  gst_sdp_message_uninit (&msg);

done:
  g_free (sdp);
  return ret;
}

static void
free_context (Context * ctx)
{
  g_object_unref (ctx->sock);
  g_free (ctx->session_name);
  if (ctx->watch_id)
    g_source_remove (ctx->watch_id);
  g_free (ctx);
}

static Context *
listen_to_announcements (const gchar * session_name, GError ** err)
{
  GSocketAddress *address, *local_address;
  GIOChannel *chan;
  Context *res = g_new0 (Context, 1);

  address =
      g_inet_socket_address_new_from_string (DEFAULT_SAP_ADDRESS,
      DEFAULT_SAP_PORT);
  local_address =
      g_inet_socket_address_new_from_string ("0.0.0.0",
      DEFAULT_SAP_PORT);

  res->session_name = g_strdup (session_name);

  if (!(res->sock =
          g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
              G_SOCKET_PROTOCOL_UDP, err)))
    goto fail;
  if (!g_socket_bind (res->sock, local_address, TRUE, err))
    goto fail;
  if (!g_socket_join_multicast_group (res->sock,
          g_inet_socket_address_get_address ((GInetSocketAddress *) address),
          FALSE, NULL, err))
    goto fail;
#ifdef G_OS_WIN32
  chan = g_io_channel_win32_new_socket (g_socket_get_fd (res->sock));
#else
  chan = g_io_channel_unix_new (g_socket_get_fd (res->sock));
#endif

  res->watch_id = g_io_add_watch (chan, G_IO_IN, sap_cb, res);
  g_io_channel_unref (chan);

done:
  g_object_unref (address);
  g_object_unref (local_address);
  return res;

fail:
  free_context (res);
  res = NULL;
  goto done;
}

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  Context *ctx;
  GError *err = NULL;

  gst_init (&argc, &argv);

  if (argc < 2) {
    g_print ("usage: %s SESSION_NAME\n" "example: %s test\n", argv[0], argv[0]);
    return -1;
  }

  loop = g_main_loop_new (NULL, FALSE);

  if (!(ctx = listen_to_announcements (argv[1], &err))) {
    g_print ("Cannot listen to announcements: %s\n", err->message);
    return -1;
  }

  g_main_loop_run (loop);

  free_context (ctx);
  g_main_loop_unref (loop);

  return 0;
}
