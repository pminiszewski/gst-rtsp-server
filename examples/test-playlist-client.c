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

/* This value is only suitable for local networks with no congestion */
#define LATENCY 40

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

static void
source_setup_cb (GstElement *playbin, GstElement *source, gpointer unused)
{
  g_object_set (source, "latency", LATENCY, NULL);
}

int
main (int argc, char *argv[])
{
  GstElement *pipe;
  GMainLoop *loop;

  gst_init (&argc, &argv);

  if (argc < 2) {
    g_print ("usage: %s rtsp://URI\n"
        "example: %s rtsp://localhost:8554/test\n",
        argv[0], argv[0]);
    return -1;
  }

  loop = g_main_loop_new (NULL, FALSE);

  pipe = gst_element_factory_make ("playbin", NULL);
  g_signal_connect (pipe, "source-setup", G_CALLBACK (source_setup_cb), NULL);
  g_object_set (pipe, "uri", argv[1], NULL);

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
