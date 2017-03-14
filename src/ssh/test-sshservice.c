/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
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

#include "cockpitsshtransport.h"
#include "cockpitsshservice.h"

#include "common/cockpitpipetransport.h"
#include "common/cockpittransport.h"
#include "common/cockpitjson.h"
#include "common/cockpittest.h"
#include "common/mock-io-stream.h"
#include "common/cockpitwebserver.h"
#include "common/cockpitconf.h"

#include <glib.h>

#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>

gint cockpit_ssh_specific_port = 0;

gint cockpit_ssh_session_timeout = 30;

#define TIMEOUT 30

#define WAIT_UNTIL(cond) \
  G_STMT_START \
    while (!(cond)) g_main_context_iteration (NULL, TRUE); \
  G_STMT_END

#define PASSWORD "this is the password"

typedef struct {
  /* setup_mock_sshd */
  const gchar *ssh_user;
  const gchar *ssh_password;
  GPid mock_sshd;
  guint16 ssh_port;

  CockpitTransport *

  /* serve_socket */
  CockpitSshService *service;
} TestCase;

typedef struct {
  const char *bridge;
} TestFixture;


static GString *
read_all_into_string (int fd)
{
  GString *input = g_string_new ("");
  gsize len;
  gssize ret;

  for (;;)
    {
      len = input->len;
      g_string_set_size (input, len + 256);
      ret = read (fd, input->str + len, 256);
      if (ret < 0)
        {
          if (errno != EAGAIN)
            {
              g_critical ("couldn't read from mock input: %s", g_strerror (errno));
              g_string_free (input, TRUE);
              return NULL;
            }
        }
      else if (ret == 0)
        {
          return input;
        }
      else
        {
          input->len = len + ret;
          input->str[input->len] = '\0';
        }
    }
}

static gboolean
start_mock_sshd (const gchar *user,
                 const gchar *password,
                 GPid *out_pid,
                 gushort *out_port)
{
  GError *error = NULL;
  GString *port;
  gchar *endptr;
  guint64 value;
  gint out_fd;

  const gchar *argv[] = {
      BUILDDIR "/mock-sshd",
      "--user", user,
      "--password", password,
      NULL
  };


  g_spawn_async_with_pipes (BUILDDIR, (gchar **)argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                            out_pid, NULL, &out_fd, NULL, &error);
  g_assert_no_error (error);

  /*
   * mock-sshd prints its port on stdout, and then closes stdout
   * This also lets us know when it has initialized.
   */

  port = read_all_into_string (out_fd);
  g_assert (port != NULL);
  close (out_fd);
  g_assert_no_error (error);

  g_strstrip (port->str);
  value = g_ascii_strtoull (port->str, &endptr, 10);
  if (!endptr || *endptr != '\0' || value == 0 || value > G_MAXUSHORT)
      g_critical ("invalid port printed by mock-sshd: %s", port->str);

  *out_port = (gushort)value;
  g_string_free (port, TRUE);
  return TRUE;
#else

  *out_pid = 0;
  *out_port = 0;
  return FALSE;
#endif
}

static void
setup_mock_sshd (TestCase *test,
                 gconstpointer data)
{
  start_mock_sshd (test->ssh_user ? test->ssh_user : g_get_user_name (),
                   test->ssh_password ? test->ssh_password : PASSWORD,
                   &test->mock_sshd,
                   &test->ssh_port);

  cockpit_ssh_specific_port = test->ssh_port;
}

static void
stop_mock_sshd (GPid mock_sshd) {
  GPid pid;
  int status;

  pid = waitpid (mock_sshd, &status, WNOHANG);
  g_assert_cmpint (pid, >=, 0);
  if (pid == 0)
    kill (mock_sshd, SIGTERM);
  else if (status != 0)
    {
      if (WIFSIGNALED (status))
        g_message ("mock-sshd terminated: %d", WTERMSIG (status));
      else
        g_message ("mock-sshd failed: %d", WEXITSTATUS (status));
    }
  g_spawn_close_pid (mock_sshd);
}

static void
teardown_mock_sshd (TestCase *test,
                    gconstpointer data)
{
  stop_mock_sshd (test->mock_sshd);

}

static void
setup (TestCase *test,
       gconstpointer data)
{
  GError *error = NULL;
  const gchar *user;
  GBytes *password;

  /* Zero port makes server choose its own */
  test->web_server = cockpit_web_server_new (NULL, 0, NULL, NULL, &error);
  g_assert_no_error (error);

  user = g_get_user_name ();
  test->auth = mock_auth_new (user, PASSWORD);

  password = g_bytes_new_take (g_strdup (PASSWORD), strlen (PASSWORD));
  test->creds = cockpit_creds_new (user, "cockpit",
                                   COCKPIT_CRED_PASSWORD, password,
                                   COCKPIT_CRED_CSRF_TOKEN, "my-csrf-token",
                                   NULL);
  g_bytes_unref (password);
}

static void
teardown_mock_webserver (TestCase *test,
                         gconstpointer data)
{
  g_clear_object (&test->web_server);
  if (test->creds)
    cockpit_creds_unref (test->creds);
  g_clear_object (&test->auth);
  g_free (test->cookie);
}

static void
setup_io_streams (TestCase *test,
                  gconstpointer data)
{
  const TestFixture *fixture = data;
  GSocket *socket1, *socket2;
  GError *error = NULL;
  int fds[2];

  if (socketpair (PF_UNIX, SOCK_STREAM, 0, fds) < 0)
    g_assert_not_reached ();

  socket1 = g_socket_new_from_fd (fds[0], &error);
  g_assert_no_error (error);

  socket2 = g_socket_new_from_fd (fds[1], &error);
  g_assert_no_error (error);

  test->io_a = G_IO_STREAM (g_socket_connection_factory_create_connection (socket1));
  test->io_b = G_IO_STREAM (g_socket_connection_factory_create_connection (socket2));

  g_object_unref (socket1);
  g_object_unref (socket2);

  if (fixture && fixture->bridge)
    cockpit_ws_bridge_program = fixture->bridge;
  else
    cockpit_ws_bridge_program = BUILDDIR "/mock-echo";
}

static void
teardown_io_streams (TestCase *test,
                     gconstpointer data)
{
  g_clear_object (&test->io_a);
  g_clear_object (&test->io_b);
}

static void
setup_for_socket (TestCase *test,
                  gconstpointer data)
{
  alarm (TIMEOUT);

  setup_mock_sshd (test, data);
  setup_mock_webserver (test, data);
  setup_io_streams (test, data);
}

static void
setup_for_socket_spec (TestCase *test,
                       gconstpointer data)
{
  test->ssh_user = "user";
  test->ssh_password = "Another password";
  setup_for_socket (test, data);
}

static void
teardown_for_socket (TestCase *test,
                     gconstpointer data)
{
  teardown_mock_sshd (test, data);
  teardown_mock_webserver (test, data);
  teardown_io_streams (test, data);

  /* Reset this if changed by a test */
  cockpit_ws_session_timeout = 30;

  cockpit_assert_expected ();
  alarm (0);
}

static gboolean
on_error_not_reached (WebSocketConnection *ws,
                      GError *error,
                      gpointer user_data)
{
  g_assert (error != NULL);

  /* At this point we know this will fail, but is informative */
  g_assert_no_error (error);
  return TRUE;
}

static gboolean
on_error_copy (WebSocketConnection *ws,
               GError *error,
               gpointer user_data)
{
  GError **result = user_data;
  g_assert (error != NULL);
  g_assert (result != NULL);
  g_assert (*result == NULL);
  *result = g_error_copy (error);
  return TRUE;
}

static gboolean
on_timeout_fail (gpointer data)
{
  g_error ("timeout during test: %s", (gchar *)data);
  return FALSE;
}

#define BUILD_INTS GINT_TO_POINTER(1)

static GBytes *
builder_to_bytes (JsonBuilder *builder)
{
  GBytes *bytes;
  gchar *data;
  gsize length;
  JsonNode *node;

  json_builder_end_object (builder);
  node = json_builder_get_root (builder);
  data = cockpit_json_write (node, &length);
  data = g_realloc (data, length + 1);
  memmove (data + 1, data, length);
  memcpy (data, "\n", 1);
  bytes = g_bytes_new_take (data, length + 1);
  json_node_free (node);
  return bytes;
}

static GBytes *
build_control_va (const gchar *command,
                  const gchar *channel,
                  va_list va)
{
  GBytes *bytes;
  JsonBuilder *builder;
  const gchar *option;
  gboolean strings = TRUE;

  builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "command");
  json_builder_add_string_value (builder, command);
  if (channel)
    {
      json_builder_set_member_name (builder, "channel");
      json_builder_add_string_value (builder, channel);
    }

  for (;;)
    {
      option = va_arg (va, const gchar *);
      if (option == BUILD_INTS)
        {
          strings = FALSE;
          option = va_arg (va, const gchar *);
        }
      if (!option)
        break;
      json_builder_set_member_name (builder, option);
      if (strings)
        json_builder_add_string_value (builder, va_arg (va, const gchar *));
      else
        json_builder_add_int_value (builder, va_arg (va, gint));
    }

  bytes = builder_to_bytes (builder);
  g_object_unref (builder);

  return bytes;
}

static void
send_control_message (WebSocketConnection *ws,
                      const gchar *command,
                      const gchar *channel,
                      ...) G_GNUC_NULL_TERMINATED;

static void
send_control_message (WebSocketConnection *ws,
                      const gchar *command,
                      const gchar *channel,
                      ...)
{
  GBytes *payload;
  va_list va;

  va_start (va, channel);
  payload = build_control_va (command, channel, va);
  va_end (va);

  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, payload);
  g_bytes_unref (payload);
}

static void
expect_control_message (GBytes *message,
                        const gchar *command,
                        const gchar *expected_channel,
                        ...) G_GNUC_NULL_TERMINATED;

static void
expect_control_message (GBytes *message,
                        const gchar *expected_command,
                        const gchar *expected_channel,
                        ...)
{
  gchar *outer_channel;
  const gchar *message_command;
  const gchar *message_channel;
  JsonObject *options;
  GBytes *payload;
  const gchar *expect_option;
  const gchar *expect_value;
  const gchar *value;
  va_list va;

  payload = cockpit_transport_parse_frame (message, &outer_channel);
  g_assert (payload != NULL);
  g_assert_cmpstr (outer_channel, ==, NULL);
  g_free (outer_channel);

  g_assert (cockpit_transport_parse_command (payload, &message_command,
                                             &message_channel, &options));
  g_bytes_unref (payload);

  g_assert_cmpstr (expected_command, ==, message_command);
  g_assert_cmpstr (expected_channel, ==, expected_channel);

  va_start (va, expected_channel);
  for (;;) {
      expect_option = va_arg (va, const gchar *);
      if (!expect_option)
        break;
      expect_value = va_arg (va, const gchar *);
      g_assert (expect_value != NULL);
      value = NULL;
      if (json_object_has_member (options, expect_option))
        value = json_object_get_string_member (options, expect_option);
      g_assert_cmpstr (value, ==, expect_value);
  }
  va_end (va);

  json_object_unref (options);
}

static void
start_web_service_and_create_client (TestCase *test,
                                     const TestFixture *fixture,
                                     WebSocketConnection **ws,
                                     CockpitWebService **service)
{
  cockpit_config_file = fixture ? fixture->config : NULL;
  const char *origin = fixture ? fixture->origin : NULL;
  if (!origin)
    origin = "http://127.0.0.1";

  /* This is web_socket_client_new_for_stream() */
  *ws = g_object_new (WEB_SOCKET_TYPE_CLIENT,
                     "url", "ws://127.0.0.1/unused",
                     "origin", origin,
                     "io-stream", test->io_a,
                     NULL);

  g_signal_connect (*ws, "error", G_CALLBACK (on_error_not_reached), NULL);
  web_socket_client_include_header (WEB_SOCKET_CLIENT (*ws), "Cookie", test->cookie);

  /* Matching the above origin */
  cockpit_ws_default_host_header = "127.0.0.1";
  cockpit_ws_default_protocol_header = fixture ? fixture->forward : NULL;

  *service = cockpit_web_service_new (test->creds, NULL);

  /* Note, we are forcing the websocket to parse its own headers */
  cockpit_web_service_socket (*service, "/unused", test->io_b, NULL, NULL);
}

static void
start_web_service_and_connect_client (TestCase *test,
                                      const TestFixture *fixture,
                                      WebSocketConnection **ws,
                                      CockpitWebService **service)
{
  GBytes *message;

  start_web_service_and_create_client (test, fixture, ws, service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (*ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (*ws) == WEB_SOCKET_STATE_OPEN);

  /* Send the open control message that starts the bridge. */
  send_control_message (*ws, "init", NULL, BUILD_INTS, "version", 1, NULL);
  send_control_message (*ws, "open", "4", "payload", "echo", NULL);

  /* This message should be echoed */
  message = g_bytes_new ("4\ntest", 6);
  web_socket_connection_send (*ws, WEB_SOCKET_DATA_TEXT, NULL, message);
  g_bytes_unref (message);
}

static void
close_client_and_stop_web_service (TestCase *test,
                                   WebSocketConnection *ws,
                                   CockpitWebService *service)
{
  guint timeout;

  if (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN)
    {
      web_socket_connection_close (ws, 0, NULL);
      WAIT_UNTIL (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_CLOSED);
    }

  g_object_unref (ws);

  /* Wait until service is done */
  timeout = g_timeout_add_seconds (20, on_timeout_fail, "closing web service");
  g_object_add_weak_pointer (G_OBJECT (service), (gpointer *)&service);
  g_object_unref (service);
  while (service != NULL)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (timeout);
  cockpit_conf_cleanup ();
}


static void
on_message_get_bytes (WebSocketConnection *ws,
                      WebSocketDataType type,
                      GBytes *message,
                      gpointer user_data)
{
  GBytes **received = user_data;
  g_assert_cmpint (type, ==, WEB_SOCKET_DATA_TEXT);
  if (*received != NULL)
    {
      gsize length;
      gconstpointer data = g_bytes_get_data (message, &length);
      g_test_message ("received unexpected extra message: %.*s", (int)length, (gchar *)data);
      g_assert_not_reached ();
    }
  *received = g_bytes_ref (message);
}

static void
on_message_get_non_control (WebSocketConnection *ws,
                            WebSocketDataType type,
                            GBytes *message,
                            gpointer user_data)
{
  GBytes **received = user_data;
  g_assert_cmpint (type, ==, WEB_SOCKET_DATA_TEXT);
  /* Control messages have this prefix: ie: a zero channel */
  if (g_str_has_prefix (g_bytes_get_data (message, NULL), "\n"))
      return;
  g_assert (*received == NULL);
  *received = g_bytes_ref (message);
}

static void
test_specified_creds (TestCase *test,
                      gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  GBytes *sent;
  CockpitWebService *service;

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  /* Open a channel with a non-standard command */
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 1, NULL);
  send_control_message (ws, "open", "4",
                        "payload", "test-text",
                        "user", "user", "password",
                        "Another password",
                        NULL);

  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_non_control), &received);

  sent = g_bytes_new_static ("4\nwheee", 7);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, sent);
  WAIT_UNTIL (received != NULL);
  g_assert (g_bytes_equal (received, sent));
  g_bytes_unref (sent);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_specified_creds_overide_host (TestCase *test,
                                   gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  GBytes *sent;
  CockpitWebService *service;

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  /* Open a channel with a host that has a bad username
     but use a good username in the json */
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 1, NULL);
  send_control_message (ws, "open", "4",
                        "payload", "test-text",
                        "user", "user", "password",
                        "Another password",
                        "host", "test@127.0.0.1",
                        NULL);

  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_non_control), &received);

  sent = g_bytes_new_static ("4\nwheee", 7);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, sent);
  WAIT_UNTIL (received != NULL);
  g_assert (g_bytes_equal (received, sent));
  g_bytes_unref (sent);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_user_host_fail (TestCase *test,
                     gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;
  const gchar *expect_problem = "authentication-failed";

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  /* Open a channel with a host that has a bad username */
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 1, NULL);
  send_control_message (ws, "open", "4",
                        "payload", "test-text",
                        "host", "baduser@127.0.0.1",
                        NULL);

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "hint", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* We should now get a close command */
  WAIT_UNTIL (received != NULL);

  /* Should have gotten a failure message, about the credentials */
  expect_control_message (received, "close", "4", "problem", expect_problem, NULL);
  g_bytes_unref (received);

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_user_host_reuse_password (TestCase *test,
                               gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  GBytes *sent;
  CockpitWebService *service;
  const gchar *user = g_get_user_name ();
  gchar *user_host = NULL;

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  /* Open a channel with the same user as creds but no password */
  user_host = g_strdup_printf ("%s@127.0.0.1", user);
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 1, NULL);
  send_control_message (ws, "open", "4",
                        "payload", "test-text",
                        "host", user_host,
                        NULL);

  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_non_control), &received);

  sent = g_bytes_new_static ("4\nwheee", 7);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, sent);
  WAIT_UNTIL (received != NULL);
  g_assert (g_bytes_equal (received, sent));
  g_bytes_unref (sent);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
  g_free (user_host);
}

static void
test_host_port (TestCase *test,
                      gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  GBytes *sent = NULL;
  CockpitWebService *service;
  gchar *host = NULL;
  GPid pid;
  gushort port;

  /* start a new mock sshd on a different port */
  start_mock_sshd ("auser", "apassword", &pid, &port);

  host = g_strdup_printf ("127.0.0.1:%d", port);

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  /* Open a channel with a host that has a port
   * and a user that doesn't work on the main mock ssh
   */
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 1, NULL);
  send_control_message (ws, "open", "4",
                        "payload", "test-text",
                        "host", host,
                        "user", "auser",
                        "password", "apassword",
                        NULL);

  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_non_control), &received);

  sent = g_bytes_new_static ("4\nwheee", 7);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, sent);
  WAIT_UNTIL (received != NULL);
  g_assert (g_bytes_equal (received, sent));
  g_bytes_unref (sent);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
  stop_mock_sshd (pid);
  g_free (host);
}

static void
test_specified_creds_fail (TestCase *test,
                           gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  /* Open a channel with a non-standard command, but a bad password */
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 1, NULL);
  send_control_message (ws, "open", "4",
                        "payload", "test-text",
                        "user", "user",
                        "password", "Wrong password",
                        NULL);

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "hint", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* We should now get a close command */
  WAIT_UNTIL (received != NULL);

  /* Should have gotten a failure message, about the credentials */
  expect_control_message (received, "close", "4", "problem", "authentication-failed", NULL);
  g_bytes_unref (received);

  close_client_and_stop_web_service (test, ws, service);
}

static const gchar MOCK_RSA_KEY[] = "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQCYzo07OA0H6f7orVun9nIVjGYrkf8AuPDScqWGzlKpAqSipoQ9oY/mwONwIOu4uhKh7FTQCq5p+NaOJ6+Q4z++xBzSOLFseKX+zyLxgNG28jnF06WSmrMsSfvPdNuZKt9rZcQFKn9fRNa8oixa+RsqEEVEvTYhGtRf7w2wsV49xIoIza/bln1ABX1YLaCByZow+dK3ZlHn/UU0r4ewpAIZhve4vCvAsMe5+6KJH8ft/OKXXQY06h6jCythLV4h18gY/sYosOa+/4XgpmBiE7fDeFRKVjP3mvkxMpxce+ckOFae2+aJu51h513S9kxY2PmKaV/JU9HBYO+yO4j+j24v";

static const gchar MOCK_RSA_FP[] = "0e:6a:c8:b1:07:72:e2:04:95:9f:0e:b3:56:af:48:e2";

static void
test_unknown_host_key (TestCase *test,
                       gconstpointer data)
{
  WebSocketConnection *ws;
  CockpitWebService *service;
  GBytes *received = NULL;
  gchar *knownhosts;

  knownhosts = g_strdup_printf ("[127.0.0.1]:%d %s", (int)test->ssh_port, MOCK_RSA_KEY);

  cockpit_expect_info ("*New connection from*");

  /* No known hosts */
  cockpit_ws_known_hosts = "/dev/null";

  start_web_service_and_connect_client (test, data, &ws, &service);
  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  /* Should get an init message */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* Should get a hint message */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "hint", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* Should close right after opening */
  while (received == NULL && web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CLOSED)
    g_main_context_iteration (NULL, TRUE);

  /* And we should have received a close message */
  g_assert (received != NULL);
  expect_control_message (received, "close", "4", "problem", "unknown-hostkey",
                          "host-key", knownhosts,
                          "host-fingerprint", MOCK_RSA_FP,
                          NULL);
  g_bytes_unref (received);
  received = NULL;

  close_client_and_stop_web_service (test, ws, service);
  g_free (knownhosts);
}

static void
test_expect_host_key (TestCase *test,
                      gconstpointer data)
{
  WebSocketConnection *ws;
  CockpitWebService *service;
  GBytes *received = NULL;
  GBytes *message;
  gulong handler;
  gchar *knownhosts;

  knownhosts = g_strdup_printf ("[127.0.0.1]:%d %s", (int)test->ssh_port, MOCK_RSA_KEY);

  /* No known hosts */
  cockpit_ws_known_hosts = "/dev/null";
  cockpit_ws_session_timeout = 1;

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 1, NULL);
  send_control_message (ws, "open", "4",
                        "payload", "test-text",
                        "host-key", knownhosts,
                        NULL);

  handler = g_signal_connect (ws, "message", G_CALLBACK (on_message_get_non_control), &received);
  message = g_bytes_new ("4\ntest", 6);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, message);

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);

  /* And we should have received the echo even though no known hosts */
  g_assert (g_bytes_equal (received, message));
  g_bytes_unref (message);
  g_bytes_unref (received);
  received = NULL;

  /* Make sure that a new channel doesn't
   * reuse the same connection. Open a new
   * channel (5) while 4 is still open.
   */
  send_control_message (ws, "open", "5",
                        "payload", "test-text",
                        NULL);

  /* Close the initial channel so mock-sshd dies */
  send_control_message (ws, "close", "4", NULL);

  g_signal_handler_disconnect (ws, handler);
  handler = g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);

  /*
   * Because our mock sshd only deals with one connection
   * channel 5 should be trying to connect to it instead of
   * reusing the same transport. When channel 4 closes and it's
   * transport get cleaned up mock-ssh will go away and channel
   * 5 will fail with a no-host error.
   */
  expect_control_message (received, "close", "5", "problem", "no-host", NULL);
  g_bytes_unref (received);
  received = NULL;

  g_signal_handler_disconnect (ws, handler);
  close_client_and_stop_web_service (test, ws, service);
  g_free (knownhosts);
}

static void
test_expect_host_key_public (TestCase *test,
                             gconstpointer data)
{
  WebSocketConnection *ws;
  CockpitWebService *service;
  GBytes *received = NULL;
  GBytes *message;
  GBytes *payload;
  JsonBuilder *builder;
  gulong handler;
  gchar *knownhosts;

  knownhosts = g_strdup_printf ("[127.0.0.1]:%d %s", (int)test->ssh_port, MOCK_RSA_KEY);

  /* No known hosts */
  cockpit_ws_known_hosts = "/dev/null";

  start_web_service_and_create_client (test, data, &ws, &service);
  WAIT_UNTIL (web_socket_connection_get_ready_state (ws) != WEB_SOCKET_STATE_CONNECTING);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);

  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 1, NULL);

  builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "command");
  json_builder_add_string_value (builder, "open");
  json_builder_set_member_name (builder, "channel");
  json_builder_add_string_value (builder, "4");
  json_builder_set_member_name (builder, "payload");
  json_builder_add_string_value (builder, "test-text");
  json_builder_set_member_name (builder, "host-key");
  json_builder_add_string_value (builder, knownhosts);
  json_builder_set_member_name (builder, "temp-session");
  json_builder_add_boolean_value (builder, FALSE);
  payload = builder_to_bytes (builder);
  g_object_unref (builder);

  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, payload);
  g_bytes_unref (payload);

  handler = g_signal_connect (ws, "message", G_CALLBACK (on_message_get_non_control), &received);
  message = g_bytes_new ("4\ntest", 6);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, message);

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);

  /* And we should have received the echo even though no known hosts */
  g_assert (g_bytes_equal (received, message));
  g_bytes_unref (received);
  received = NULL;
  g_bytes_unref (message);
  message = NULL;

  /* Open another channel without the host-key */
  send_control_message (ws, "open", "a", "payload", "echo", NULL);
  message = g_bytes_new ("a\ntest", 6);
  web_socket_connection_send (ws, WEB_SOCKET_DATA_TEXT, NULL, message);

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);

  /* And we should have received the echo even though no known hosts */
  g_assert (g_bytes_equal (received, message));
  g_bytes_unref (message);
  g_bytes_unref (received);
  received = NULL;

  g_signal_handler_disconnect (ws, handler);
  close_client_and_stop_web_service (test, ws, service);
  g_free (knownhosts);
}


static void
test_auth_results (TestCase *test,
                   gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;
  JsonObject *options;
  JsonObject *auth_results;
  GBytes *payload;
  gchar *ochannel = NULL;
  const gchar *channel;
  const gchar *command;

  /* Fail to spawn this program */
  cockpit_ws_bridge_program = "/nonexistant";

  start_web_service_and_connect_client (test, data, &ws, &service);
  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);
  g_signal_handlers_disconnect_by_func (ws, on_error_not_reached, NULL);

  /* Should get an init message */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* A hint from the other end */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "hint", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* Channel should close immediately */
  WAIT_UNTIL (received != NULL);

  /* Should have auth methods details */
  payload = cockpit_transport_parse_frame (received, &ochannel);
  g_bytes_unref (received);
  received = NULL;

  g_assert (payload != NULL);
  g_free (ochannel);

  g_assert (cockpit_transport_parse_command (payload, &command, &channel, &options));
  g_bytes_unref (payload);

  g_assert_cmpstr (command, ==, "close");
  g_assert_cmpstr (json_object_get_string_member (options, "problem"), ==, "no-cockpit");

  auth_results = json_object_get_object_member (options, "auth-method-results");
  g_assert (auth_results != NULL);

  if (json_object_has_member (auth_results, "public-key"))
    g_assert_cmpstr ("denied", ==,
                     json_object_get_string_member (auth_results, "public-key"));

  g_assert_cmpstr ("succeeded", ==,
                   json_object_get_string_member (auth_results, "password"));
  g_assert_cmpstr ("no-server-support", ==,
                   json_object_get_string_member (auth_results, "gssapi-mic"));

  json_object_unref (options);
  g_bytes_unref (received);

  close_client_and_stop_web_service (test, ws, service);
}

static void
test_fail_spawn (TestCase *test,
                 gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;

  /* Fail to spawn this program */
  cockpit_ws_bridge_program = "/nonexistant";

  start_web_service_and_connect_client (test, data, &ws, &service);
  g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);
  g_signal_handlers_disconnect_by_func (ws, on_error_not_reached, NULL);

  /* Should get an init message */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "hint", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* Channel should close immediately */
  WAIT_UNTIL (received != NULL);

  /* But we should have gotten failure message, about the spawn */
  expect_control_message (received, "close", "4", "problem", "no-cockpit", NULL);
  g_bytes_unref (received);

  close_client_and_stop_web_service (test, ws, service);
}

static const TestFixture fixture_kill_group = {
    .bridge = BUILDDIR "/cockpit-bridge"
};

static void
test_kill_host (TestCase *test,
                gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;
  GHashTable *seen;
  gchar *ochannel;
  const gchar *channel;
  const gchar *command;
  JsonObject *options;
  GBytes *payload;
  gulong handler;

  /* Sends a "test" message in channel "4" */
  start_web_service_and_connect_client (test, data, &ws, &service);

  handler = g_signal_connect (ws, "message", G_CALLBACK (on_message_get_non_control), &received);

  /* Drain the initial message */
  WAIT_UNTIL (received != NULL);
  g_bytes_unref (received);
  received = NULL;

  g_signal_handler_disconnect (ws, handler);
  handler = g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  seen = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_add (seen, "a");
  g_hash_table_add (seen, "b");
  g_hash_table_add (seen, "c");
  g_hash_table_add (seen, "4");

  send_control_message (ws, "open", "a", "payload", "echo", "group", "test", NULL);
  send_control_message (ws, "open", "b", "payload", "echo", "group", "test", NULL);
  send_control_message (ws, "open", "c", "payload", "echo", "group", "test", NULL);

  /* Kill all the above channels */
  send_control_message (ws, "kill", NULL, "host", "localhost", NULL);

  /* All the close messages */
  while (g_hash_table_size (seen) > 0)
    {
      WAIT_UNTIL (received != NULL);

      payload = cockpit_transport_parse_frame (received, &ochannel);
      g_bytes_unref (received);
      received = NULL;

      g_assert (payload != NULL);
      g_assert_cmpstr (ochannel, ==, NULL);
      g_free (ochannel);

      g_assert (cockpit_transport_parse_command (payload, &command, &channel, &options));
      g_bytes_unref (payload);

      if (!g_str_equal (command, "open") && !g_str_equal (command, "ready"))
        {
          g_assert_cmpstr (command, ==, "close");
          g_assert_cmpstr (json_object_get_string_member (options, "problem"), ==, "terminated");
          g_assert (g_hash_table_remove (seen, channel));
        }
      json_object_unref (options);
    }

  g_hash_table_destroy (seen);

  g_signal_handler_disconnect (ws, handler);

  close_client_and_stop_web_service (test, ws, service);
}

static gboolean
on_timeout_dummy (gpointer unused)
{
  return TRUE;
}

static void
test_timeout_session (TestCase *test,
                      gconstpointer data)
{
  WebSocketConnection *ws;
  GBytes *received = NULL;
  CockpitWebService *service;
  GError *error = NULL;
  JsonObject *object;
  GBytes *payload;
  gchar *unused;
  pid_t pid;
  guint sig;
  guint tag;

  cockpit_ws_session_timeout = 1;

  /* This sends us a mesage with a pid in it on channel ' ' */
  cockpit_ws_bridge_program = SRCDIR "/src/ws/mock-pid-cat";

  /* Start the client */
  start_web_service_and_create_client (test, data, &ws, &service);
  while (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_CONNECTING)
    g_main_context_iteration (NULL, TRUE);
  g_assert (web_socket_connection_get_ready_state (ws) == WEB_SOCKET_STATE_OPEN);
  sig = g_signal_connect (ws, "message", G_CALLBACK (on_message_get_bytes), &received);

  /* Queue channel open/close, so we can guarantee having a session */
  send_control_message (ws, "init", NULL, BUILD_INTS, "version", 1, NULL);
  send_control_message (ws, "open", "11x", "payload", "test-text", NULL);

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "init", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);
  expect_control_message (received, "hint", NULL, NULL);
  g_bytes_unref (received);
  received = NULL;

  /* First we should receive the pid message from mock-pid-cat */
  while (received == NULL)
    g_main_context_iteration (NULL, TRUE);

  payload = cockpit_transport_parse_frame (received, &unused);
  g_assert (payload);
  g_bytes_unref (received);
  g_free (unused);

  object = cockpit_json_parse_bytes (payload, &error);
  g_assert_no_error (error);
  pid = json_object_get_int_member (object, "pid");
  json_object_unref (object);
  g_bytes_unref (payload);

  g_signal_handler_disconnect (ws, sig);

  send_control_message (ws, "close", "11x", NULL);

  /* The process should exit shortly */
  tag = g_timeout_add_seconds (1, on_timeout_dummy, NULL);
  while (kill (pid, 0) == 0)
    g_main_context_iteration (NULL, TRUE);
  g_source_remove (tag);

  g_assert_cmpint (errno, ==, ESRCH);

  close_client_and_stop_web_service (test, ws, service);
}


static gboolean
on_hack_raise_sigchld (gpointer user_data)
{
  raise (SIGCHLD);
  return TRUE;
}

int
main (int argc,
      char *argv[])
{
  gchar *name;
  gint i;

  cockpit_test_init (&argc, &argv);
  cockpit_ssh_program = BUILDDIR "/cockpit-ssh";

  /*
   * HACK: Work around races in glib SIGCHLD handling.
   *
   * https://bugzilla.gnome.org/show_bug.cgi?id=731771
   * https://bugzilla.gnome.org/show_bug.cgi?id=711090
   */
  g_timeout_add_seconds (1, on_hack_raise_sigchld, NULL);

  /* Try to debug crashing during tests */
  signal (SIGSEGV, cockpit_test_signal_backtrace);

  g_test_add ("/ssh-service/auth-results", TestCase,
              NULL, setup,
              test_auth_results, teardown);
  g_test_add ("/ssh-service/fail-spawn", TestCase,
              &fixture_rfc6455, setup,
              test_fail_spawn, teardown);

  g_test_add ("/web-service/kill-host", TestCase, &fixture_kill_group,
              setup, test_kill_host, teardown);

  g_test_add ("/web-service/specified-creds", TestCase,
              &fixture_rfc6455, setup,
              test_specified_creds, teardown);
  g_test_add ("/web-service/specified-creds-fail", TestCase,
              &fixture_rfc6455, setup,
              test_specified_creds_fail, teardown);
  g_test_add ("/web-service/specified-creds-overide-host", TestCase,
              &fixture_rfc6455, setup,
              test_specified_creds_overide_host, teardown);
  g_test_add ("/web-service/user-host-same", TestCase,
              &fixture_rfc6455, setup,
              test_user_host_reuse_password, teardown);
  g_test_add ("/web-service/user-host-fail", TestCase,
              &fixture_rfc6455, setup,
              test_user_host_fail, teardown_for_socket);
  g_test_add ("/web-service/host-port", TestCase,
              &fixture_rfc6455, setup,
              test_host_port, teardown);

  g_test_add ("/web-service/timeout-session", TestCase, NULL,
              setup, test_timeout_session, teardownt);

  return g_test_run ();
}
