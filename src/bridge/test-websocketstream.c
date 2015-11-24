
/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2015 Red Hat, Inc.
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

#include "cockpitwebsocketstream.h"

#include "common/cockpittest.h"
#include "common/cockpitwebresponse.h"
#include "common/cockpitwebserver.h"

#include "mock-transport.h"

static void
on_closed_set_flag (CockpitChannel *channel,
                    const gchar *problem,
                    gpointer user_data)
{
  gboolean *flag = user_data;
  g_assert (flag != NULL);
  g_assert (*flag != TRUE);
  *flag = TRUE;
}

typedef struct {
  MockTransport *transport;
  CockpitWebServer *server;
  GIOStream *client;
  guint port;
} TestCase;

static void
on_socket_message (WebSocketConnection *self,
                   WebSocketDataType type,
                   GBytes *message,
                   gpointer user_data)
{
  GByteArray *array = g_bytes_unref_to_array (g_bytes_ref (message));
  GBytes *payload;
  guint i;

  /* Capitalize and relay back */
  for (i = 0; i < array->len; i++)
    array->data[i] = g_ascii_toupper (array->data[i]);

  payload = g_byte_array_free_to_bytes (array);
  web_socket_connection_send (self, type, NULL, payload);
  g_bytes_unref (payload);
}

static void
on_socket_close (WebSocketConnection *ws,
                 gpointer user_data)
{
  g_object_unref (ws);
}

static gboolean
handle_socket (CockpitWebServer *server,
               const gchar *path,
               GIOStream *io_stream,
               GHashTable *headers,
               GByteArray *input,
               gpointer data)
{
  const gchar *protocols[] = { "one", "two", "three", NULL };
  gchar *origins[2] = { NULL, NULL };
  TestCase *test;
  gchar *url;

  if (!g_str_equal (path, "/socket"))
    return FALSE;

  url = g_strdup_printf ("ws://127.0.0.1:%u/socket", test->port);
  origins = g_strdup_printf ("127.0.0.1:%u", test->port);

  ws = web_socket_server_new_for_stream (url, origins, protocols, io_stream, headers, input);

  g_signal_connect (ws, "messsage", G_CALLBACK (on_socket_message), NULL);
  g_signal_connect (ws, "close", G_CALLBACK (on_socket_close), NULL);
}


static void
setup (TestCase *test,
       gconstpointer data)
{
  test->server = cockpit_web_server_new (0, NULL, NULL, NULL, NULL);
  test->port = cockpit_web_server_get_port (tt->web_server);
  test->transport = mock_transport_new ();
}

static void
teardown (TestCase *test,
          gconstpointer data)
{
  g_object_unref (test->server);
  g_object_unref (test->transport);

  cockpit_assert_expected ();
}

static void
test_basic (TestCase *test,
            gconstpointer data)
{
  JsonObject *options;
  CockpitChannel *channel;

  options = json_object_new ();
  json_object_set_int_member (options, "port", test->port);
  json_object_set_string_member (options, "payload", "websocket-stream1");
  json_object_set_string_member (options, "path", "/socket");

  channel = g_object_new (COCKPIT_TYPE_HTTP_STREAM,
                          "transport", test->transport,
                          "id", "444",
                          "options", options,
                          NULL);

  json_object_unref (options);

  /* Tell HTTP we have no more data to send */
  bytes = g_bytes_new_static ("abcd efg", 8);
  cockpit_transport_emit_recv (COCKPIT_TRANSPORT (tt->transport), "444", bytes);
  g_bytes_unref (bytes);

  xxxx xxxx sockets xxx

  g_object_unref (channel);
}

int
main (int argc,
      char *argv[])
{
  cockpit_test_init (&argc, &argv);

  g_test_add ("/websocket-stream/basic", TestCase, NULL,
              setup, test_basic, teardown);

  return g_test_run ();
}
