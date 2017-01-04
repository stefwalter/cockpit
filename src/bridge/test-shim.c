/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2017 Red Hat, Inc.
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

#include "cockpitchannel.h"
#include "cockpitrouter.h"
#include "cockpitshim.h"
#include "mock-transport.h"

#include "common/cockpitjson.h"
#include "common/cockpittest.h"

#include <json-glib/json-glib.h>

#include <gio/gio.h>

#include <string.h>

/* Mock override from cockpitrouter.c */
extern guint cockpit_shim_bridge_timeout;
extern gchar **cockpit_shim_argv;

gchar *payload_arg;

typedef struct {
  MockTransport *transport;
} TestCase;

static void
setup (TestCase *tc,
       gconstpointer unused)
{
  const gchar *data;
  GBytes *bytes;

  static gchar *argv[] = {
    BUILDDIR "/mock-bridge", NULL, NULL
  };

  tc->transport = g_object_new (mock_transport_get_type (), NULL);
  while (g_main_context_iteration (NULL, FALSE));

  cockpit_shim_argv = argv;

  /* Pretend to have received an init message */
  data = "{\"command\":\"init\",\"host\":\"localhost\",\"version\":1}";
  bytes = g_bytes_new_static (data, strlen (data));
  cockpit_shim_reset (bytes);
  g_bytes_unref (bytes);
}

static void
teardown (TestCase *tc,
          gconstpointer unused)
{
  cockpit_assert_expected ();

  g_object_add_weak_pointer (G_OBJECT (tc->transport), (gpointer *)&tc->transport);
  g_object_unref (tc->transport);
  g_assert (tc->transport == NULL);

  g_free (payload_arg);
  payload_arg = cockpit_shim_argv[1] = NULL;
}

static GType
mock_shim (JsonObject *options)
{
  const gchar *payload;

  if (!cockpit_json_get_string (options, "payload", NULL, &payload))
    payload = NULL;
  g_free (payload_arg);
  payload_arg = g_strdup_printf ("--%s", payload);
  cockpit_shim_argv[1] = payload_arg;

  return COCKPIT_TYPE_SHIM;
}

static void
emit_string (TestCase *tc,
             const gchar *channel,
             const gchar *string)
{
  GBytes *bytes = g_bytes_new (string, strlen (string));
  cockpit_transport_emit_recv (COCKPIT_TRANSPORT (tc->transport), channel, bytes);
  g_bytes_unref (bytes);
}

static void
on_transport_closed (CockpitTransport *transport,
                     const gchar *problem,
                     gpointer user_data)
{
  gchar **retval = user_data;
  g_assert (*retval == NULL);
  *retval = g_strdup (problem);
}

static void
test_external_bridge (TestCase *tc,
                      gconstpointer unused)
{
  CockpitRouter *router;
  GBytes *sent;
  JsonObject *control;
  CockpitTransport *shim_transport = NULL;
  gchar *problem = NULL;

  /* Same argv as used by mock_shim */
  static char *argv[] = {
    BUILDDIR "/mock-bridge", "--upper", NULL
  };

  cockpit_shim_bridge_timeout = 1;
  router = cockpit_router_new (COCKPIT_TRANSPORT (tc->transport), mock_shim, "localhost");

  emit_string (tc, NULL, "{\"command\": \"open\", \"channel\": \"a\", \"payload\": \"upper\"}");
  emit_string (tc, NULL, "{\"command\": \"open\", \"channel\": \"b\", \"payload\": \"upper\"}");

  while ((control = mock_transport_pop_control (tc->transport)) == NULL)
    g_main_context_iteration (NULL, TRUE);

  cockpit_assert_json_eq (control, "{\"command\":\"ready\",\"channel\":\"a\"}");
  control = NULL;

  while ((control = mock_transport_pop_control (tc->transport)) == NULL)
    g_main_context_iteration (NULL, TRUE);

  cockpit_assert_json_eq (control, "{\"command\":\"ready\",\"channel\":\"b\"}");
  control = NULL;

  emit_string (tc, "a", "oh marmalade a");
  while ((sent = mock_transport_pop_channel (tc->transport, "a")) == NULL)
    g_main_context_iteration (NULL, TRUE);
  cockpit_assert_bytes_eq (sent, "OH MARMALADE A", -1);
  sent = NULL;

  /* Get a ref of the shim transport */
  shim_transport = cockpit_shim_ensure (NULL, argv);
  g_signal_connect (shim_transport, "closed", G_CALLBACK (on_transport_closed), &problem);

  emit_string (tc, NULL, "{\"command\": \"close\", \"channel\": \"a\" }");
  emit_string (tc, "b", "oh marmalade b");

  while ((control = mock_transport_pop_control (tc->transport)) == NULL)
    g_main_context_iteration (NULL, TRUE);
  cockpit_assert_json_eq (control, "{\"command\":\"close\",\"channel\":\"a\"}");
  control = NULL;

  while ((sent = mock_transport_pop_channel (tc->transport, "b")) == NULL)
    g_main_context_iteration (NULL, TRUE);
  cockpit_assert_bytes_eq (sent, "OH MARMALADE B", -1);
  g_assert_null (problem);
  sent = NULL;

  emit_string (tc, NULL, "{\"command\": \"close-later\", \"channel\": \"b\" }");

  while ((control = mock_transport_pop_control (tc->transport)) == NULL)
    g_main_context_iteration (NULL, TRUE);
  cockpit_assert_json_eq (control, "{\"command\":\"close\",\"channel\":\"b\",\"problem\":\"closed\"}");
  control = NULL;

  while (problem == NULL)
    g_main_context_iteration (NULL, TRUE);

  g_assert_cmpstr (problem, ==, "timeout");

  g_object_unref (router);
  g_object_unref (shim_transport);
  g_free (problem);
}

static void
test_external_fail (TestCase *tc,
                    gconstpointer unused)
{
  CockpitRouter *router;
  JsonObject *received;

  router = cockpit_router_new (COCKPIT_TRANSPORT (tc->transport), mock_shim, "localhost");

  emit_string (tc, NULL, "{\"command\": \"open\", \"channel\": \"a\", \"payload\": \"bad\"}");
  emit_string (tc, "a", "oh marmalade");

  while ((received = mock_transport_pop_control (tc->transport)) == NULL)
    g_main_context_iteration (NULL, TRUE);

  cockpit_assert_json_eq (received, "{\"command\": \"close\", \"channel\": \"a\", \"problem\": \"terminated\"}");

  g_object_unref (router);
}

static void
test_external_ensure_bridge (TestCase *tc,
                             gconstpointer unused)
{
  CockpitTransport *shim_transport1;
  CockpitTransport *shim_transport2;
  CockpitTransport *shim_transport3;

  static gchar *argv1[] = {
    BUILDDIR "/mock-bridge", "--lower", NULL
  };

  static gchar *argv2[] = {
    BUILDDIR "/mock-bridge", "--upper", NULL
  };

  shim_transport1 = cockpit_shim_ensure (NULL, argv1);
  shim_transport2 = cockpit_shim_ensure (NULL, argv1);
  shim_transport3 = cockpit_shim_ensure (NULL, argv2);

  g_assert_true (shim_transport1 == shim_transport2);
  g_assert_true (shim_transport1 != shim_transport3);

  g_object_unref (shim_transport1);
  g_object_unref (shim_transport2);
  g_object_unref (shim_transport3);
}

int
main (int argc,
      char *argv[])
{
  cockpit_test_init (&argc, &argv);

  g_test_add ("/router/external-bridge", TestCase, NULL,
              setup, test_external_bridge, teardown);
  g_test_add ("/router/external-fail", TestCase, NULL,
              setup, test_external_fail, teardown);
  g_test_add ("/router/external-ensure-bridge", TestCase, NULL,
              setup, test_external_ensure_bridge, teardown);
  return g_test_run ();
}
