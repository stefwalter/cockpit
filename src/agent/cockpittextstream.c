/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2014 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "cockpittextstream.h"

#include "cockpit/cockpitpipe.h"
#include "cockpit/cockpitunixfd.h"

#include <glib-unix.h>
#include <gio/gunixsocketaddress.h>

#include <sys/wait.h>
#include <errno.h>

/**
 * CockpitTextStream:
 *
 * A #CockpitChannel that sends messages from a regular socket
 * or file descriptor. Any data is read in whatever chunks it
 * shows up in read().
 *
 * Only UTF8 text data is transmitted. Anything else is
 * forced into UTF8 by replacing invalid characters.
 *
 * The payload type for this channel is 'text-stream'.
 */

#define COCKPIT_TEXT_STREAM(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_TEXT_STREAM, CockpitTextStream))

typedef struct {
  CockpitChannel parent;
  CockpitPipe *pipe;
  GSocket *sock;
  const gchar *name;
  gboolean open;
  gboolean closing;
  guint sig_read;
  guint sig_close;

  /* Dealing with sudo */
  gboolean with_sudo;
  gboolean had_output;
  gboolean askpass_complaint;
  int stderr_fd;
  guint stderr_watch;
} CockpitTextStream;

typedef struct {
  CockpitChannelClass parent_class;
} CockpitTextStreamClass;

G_DEFINE_TYPE (CockpitTextStream, cockpit_text_stream, COCKPIT_TYPE_CHANNEL);

static GBytes *
check_utf8_and_force_if_necessary (GBytes *input)
{
  const gchar *data;
  const gchar *end;
  gsize length;
  GString *string;

  data = g_bytes_get_data (input, &length);
  if (g_utf8_validate (data, length, &end))
    return g_bytes_ref (input);

  string = g_string_sized_new (length + 16);
  do
    {
      /* Valid part of the string */
      g_string_append_len (string, data, end - data);

      /* Replacement character */
      g_string_append (string, "\xef\xbf\xbd");

      length -= (end - data) + 1;
      data = end + 1;
    }
  while (!g_utf8_validate (data, length, &end));

  if (length)
    g_string_append_len (string, data, length);

  return g_string_free_to_bytes (string);
}

static void
cockpit_text_stream_recv (CockpitChannel *channel,
                          GBytes *message)
{
  CockpitTextStream *self = COCKPIT_TEXT_STREAM (channel);
  GBytes *clean;

  clean = check_utf8_and_force_if_necessary (message);
  cockpit_pipe_write (self->pipe, clean);
  g_bytes_unref (clean);
}

static void
cockpit_text_stream_close (CockpitChannel *channel,
                           const gchar *problem)
{
  CockpitTextStream *self = COCKPIT_TEXT_STREAM (channel);

  self->closing = TRUE;

  /*
   * If closed, call base class handler directly. Otherwise ask
   * our pipe to close first, which will come back here.
  */
  if (self->open)
    cockpit_pipe_close (self->pipe, problem);
  else
    COCKPIT_CHANNEL_CLASS (cockpit_text_stream_parent_class)->close (channel, problem);
}

static void
on_pipe_read (CockpitPipe *pipe,
              GByteArray *data,
              gboolean end_of_data,
              gpointer user_data)
{
  CockpitTextStream *self = user_data;
  CockpitChannel *channel = user_data;
  GBytes *message;
  GBytes *clean;

  if (data->len || !end_of_data)
    {
      self->had_output = TRUE;

      /* When array is reffed, this just clears byte array */
      g_byte_array_ref (data);
      message = g_byte_array_free_to_bytes (data);
      clean = check_utf8_and_force_if_necessary (message);
      cockpit_channel_send (channel, clean);
      g_bytes_unref (message);
      g_bytes_unref (clean);
    }

  /* Close the pipe when writing is done */
  if (end_of_data && self->open)
    {
      g_debug ("%s: end of data, closing pipe", self->name);
      cockpit_pipe_close (pipe, NULL);
    }
}

static void
on_pipe_close (CockpitPipe *pipe,
               const gchar *problem,
               gpointer user_data)
{
  CockpitTextStream *self = user_data;
  CockpitChannel *channel = user_data;
  gint status;

  self->open = FALSE;

  if (cockpit_pipe_get_pid (pipe, NULL))
    {
      status = cockpit_pipe_exit_status (pipe);
      if (WIFEXITED (status))
        {
          if (!problem && self->with_sudo && self->askpass_complaint)
            problem = "not-authorized";
          cockpit_channel_close_int_option (channel, "exit-status", WEXITSTATUS (status));
        }
      else if (WIFSIGNALED (status))
        cockpit_channel_close_int_option (channel, "exit-signal", WTERMSIG (status));
      else if (status)
        cockpit_channel_close_int_option (channel, "exit-status", -1);
    }

  cockpit_channel_close (channel, problem);
}

static gboolean
on_spawn_stderr (gint fd,
                 GIOCondition cond,
                 gpointer user_data)
{
  CockpitTextStream *self = user_data;
  char buffer[1024];
  gssize res;

  if (cond & G_IO_IN)
    {
      res = read (fd, buffer, sizeof (buffer));
      if (res < 0)
        {
          if (errno == EAGAIN || errno == EINTR)
            return TRUE;
          self->stderr_watch = 0;
          g_warning ("couldn't read from spawned process stderr: %s", g_strerror (errno));
          return FALSE;
        }
      else
        {
          /*
           * If sudo is complaining about a missing 'askpass' that means it's
           * trying to prompt for a password, but isn't successful. We want to
           * return 'not-authorized' in that case.
           */
          if (self->with_sudo && !self->had_output &&
              g_strstr_len (buffer, res, "askpass") == 0)
            self->askpass_complaint = TRUE;

          /* Write to our stderr */
          write (2, buffer, res);
        }
    }

  if (cond & G_IO_HUP)
    {
      self->stderr_watch = 0;
      g_debug ("spawned process stderr closed");
      return FALSE;
    }

  return TRUE;
}

static gboolean
on_idle_protocol_error (gpointer user_data)
{
  CockpitChannel *channel = COCKPIT_CHANNEL (user_data);
  cockpit_channel_close (channel, "protocol-error");
  return FALSE;
}

static void
cockpit_text_stream_init (CockpitTextStream *self)
{
  self->stderr_fd = -1;
}

static void
cockpit_text_stream_constructed (GObject *object)
{
  CockpitTextStream *self = COCKPIT_TEXT_STREAM (object);
  CockpitChannel *channel = COCKPIT_CHANNEL (self);
  GSocketAddress *address;
  const gchar *unix_path;
  const gchar **argv;
  const gchar **env;

  G_OBJECT_CLASS (cockpit_text_stream_parent_class)->constructed (object);

  unix_path = cockpit_channel_get_option (channel, "unix");
  argv = cockpit_channel_get_strv_option (channel, "spawn");

  if (argv[0] == NULL)
    argv = NULL;

  if (argv == NULL && unix_path == NULL)
    {
      g_warning ("did not receive a unix or spawn option");
      g_idle_add_full (G_PRIORITY_DEFAULT, on_idle_protocol_error,
                       g_object_ref (channel), g_object_unref);
      return;
    }
  else if (argv != NULL && unix_path != NULL)
    {
      g_warning ("received both a unix and spawn option");
      g_idle_add_full (G_PRIORITY_DEFAULT, on_idle_protocol_error,
                       g_object_ref (channel), g_object_unref);
      return;
    }
  else if (unix_path)
    {
      self->name = unix_path;
      address = g_unix_socket_address_new (unix_path);
      self->pipe = cockpit_pipe_connect (self->name, address);
      g_object_unref (address);
    }
  else if (argv)
    {
      self->name = argv[0];

      if (g_str_has_suffix (argv[0], "sudo"))
        {
          if (argv[1])
            self->name = argv[1];
          self->with_sudo = TRUE;
        }

      env = cockpit_channel_get_strv_option (channel, "environ");
      if (cockpit_channel_get_bool_option (channel, "pty"))
        self->pipe = cockpit_pipe_pty (argv, env, NULL);
      else
        self->pipe = cockpit_pipe_spawn (argv, env, NULL, &self->stderr_fd);

      if (self->stderr_fd != -1)
        {
          if (!g_unix_set_fd_nonblocking (self->stderr_fd, TRUE, NULL))
            g_warning ("couldn't set spawned process stderr to non-blocking");
          self->stderr_watch = cockpit_unix_fd_add (self->stderr_fd, G_IO_IN,
                                                    on_spawn_stderr, self);
        }
    }


  self->sig_read = g_signal_connect (self->pipe, "read", G_CALLBACK (on_pipe_read), self);
  self->sig_close = g_signal_connect (self->pipe, "close", G_CALLBACK (on_pipe_close), self);
  self->open = TRUE;
  cockpit_channel_ready (channel);
}

static void
cockpit_text_stream_dispose (GObject *object)
{
  CockpitTextStream *self = COCKPIT_TEXT_STREAM (object);

  if (self->stderr_watch)
    {
      g_source_remove (self->stderr_watch);
      self->stderr_watch = 0;
    }
  if (self->stderr_fd != -1)
    {
      close (self->stderr_fd);
      self->stderr_fd = -1;
    }
  if (self->pipe)
    {
      if (self->open)
        cockpit_pipe_close (self->pipe, "terminated");
      if (self->sig_read)
        g_signal_handler_disconnect (self->pipe, self->sig_read);
      if (self->sig_close)
        g_signal_handler_disconnect (self->pipe, self->sig_close);
      self->sig_read = self->sig_close = 0;
    }

  G_OBJECT_CLASS (cockpit_text_stream_parent_class)->dispose (object);
}

static void
cockpit_text_stream_finalize (GObject *object)
{
  CockpitTextStream *self = COCKPIT_TEXT_STREAM (object);

  g_clear_object (&self->sock);
  g_clear_object (&self->pipe);

  G_OBJECT_CLASS (cockpit_text_stream_parent_class)->finalize (object);
}

static void
cockpit_text_stream_class_init (CockpitTextStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  CockpitChannelClass *channel_class = COCKPIT_CHANNEL_CLASS (klass);

  gobject_class->constructed = cockpit_text_stream_constructed;
  gobject_class->dispose = cockpit_text_stream_dispose;
  gobject_class->finalize = cockpit_text_stream_finalize;

  channel_class->recv = cockpit_text_stream_recv;
  channel_class->close = cockpit_text_stream_close;
}

/**
 * cockpit_text_stream_open:
 * @transport: the transport to send/receive messages on
 * @channel_id: the channel id
 * @unix_path: the UNIX socket path to communicate with
 *
 * This function is mainly used by tests. The usual way
 * to get a #CockpitTextStream is via cockpit_channel_open()
 *
 * Returns: (transfer full): the new channel
 */
CockpitChannel *
cockpit_text_stream_open (CockpitTransport *transport,
                          const gchar *channel_id,
                          const gchar *unix_path)
{
  CockpitChannel *channel;
  JsonObject *options;

  g_return_val_if_fail (channel_id != NULL, NULL);

  options = json_object_new ();
  json_object_set_string_member (options, "unix", unix_path);
  json_object_set_string_member (options, "payload", "text-stream");

  channel = g_object_new (COCKPIT_TYPE_TEXT_STREAM,
                          "transport", transport,
                          "id", channel_id,
                          "options", options,
                          NULL);

  json_object_unref (options);
  return channel;
}
