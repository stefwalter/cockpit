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

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>

#include <dirent.h>
#include <string.h>

#include "cockpitws.h"

#include "cockpitcertificate.h"
#include "cockpithandlers.h"
#include "cockpitbranding.h"

#include "common/cockpitassets.h"
#include "common/cockpitconf.h"
#include "common/cockpitlog.h"
#include "common/cockpitmemory.h"
#include "common/cockpitsystem.h"
#include "common/cockpittest.h"

/* ---------------------------------------------------------------------------------------------------- */

static gint      opt_port         = 9090;
static gchar     *opt_address     = NULL;
static gchar     *opt_request     = NULL;
static gboolean  opt_no_tls       = FALSE;
static gboolean  opt_local_ssh    = FALSE;
static gchar     *opt_local_session = NULL;
static gboolean  opt_version      = FALSE;

static GOptionEntry cmd_entries[] = {
  {"port", 'p', 0, G_OPTION_ARG_INT, &opt_port, "Local port to bind to (9090 if unset)", NULL},
  {"address", 'a', 0, G_OPTION_ARG_STRING, &opt_address, "Address to bind to (binds on all addresses if unset)", "ADDRESS"},
  {"no-tls", 0, 0, G_OPTION_ARG_NONE, &opt_no_tls, "Don't use TLS", NULL},
  {"local-ssh", 0, 0, G_OPTION_ARG_NONE, &opt_local_ssh, "Log in locally via SSH", NULL },
  {"local-session", 0, 0, G_OPTION_ARG_STRING, &opt_local_session,
      "Launch a bridge in the local session (path to cockpit-bridge or '-' for stdin/out); implies --no-tls",
      "BRIDGE" },
  {"version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Print version information", NULL },
  {"request", 0, 0, G_OPTION_ARG_FILENAME, &opt_request, "Fuzzing request file", NULL},
  {NULL}
};

/* ---------------------------------------------------------------------------------------------------- */

static void
print_version (void)
{
  g_print ("Version: %s\n", PACKAGE_VERSION);
  g_print ("Protocol: 1\n");
  g_print ("Authorization: crypt1\n");
}

static gchar **
setup_static_roots (GHashTable *os_release)
{
  gchar **roots;
  const gchar *os_variant_id;
  const gchar *os_id;
  const gchar *os_id_like;

  if (os_release)
    {
      os_id = g_hash_table_lookup (os_release, "ID");
      os_variant_id = g_hash_table_lookup (os_release, "VARIANT_ID");
      os_id_like = g_hash_table_lookup (os_release, "ID_LIKE");
    }
  else
    {
      os_id = NULL;
      os_variant_id = NULL;
      os_id_like = NULL;
    }

  roots = cockpit_branding_calculate_static_roots (os_id, os_variant_id, os_id_like, TRUE);

  /* Load the fail template */
  g_resources_register (cockpitassets_get_resource ());
  cockpit_web_failure_resource = "/org/cockpit-project/Cockpit/fail.html";

  return roots;
}

static void
on_local_ready (GObject *object,
                GAsyncResult *result,
                gpointer data)
{
  cockpit_web_server_start (COCKPIT_WEB_SERVER (data));
  g_object_unref (data);
}

static gpointer
perform_http_request (gpointer loop)
{
  GSocketClient *client;
  GSocketConnection *conn;
  GInputStream *input;
  GError *error = NULL;
  gchar *request = NULL;
  gsize length = 0;
  gchar buf[1024];
  gssize ret;

#if 0
  int fd = open("/tmp/log", O_CREAT | O_RDWR, S_IRWXU);
  g_assert (fd > -1);
  g_assert (dup2 (fd, 2) > -1);
#endif

  client = g_socket_client_new ();

  g_file_get_contents (opt_request, &request, &length, &error);
  if (error)
    {
      g_printerr ("%s: %s\n", opt_request, error->message);
      abort ();
    }
  conn = g_socket_client_connect_to_host (client, opt_address ? opt_address : "127.0.0.1", opt_port, NULL, &error);
  if (error)
    {
      g_printerr ("connect: %s\n", error->message);
      abort ();
    }

  g_output_stream_write_all (g_io_stream_get_output_stream (G_IO_STREAM (conn)),
                             request, strlen (request), NULL, NULL, &error);
  if (error)
    {
      g_printerr ("write: %s\n", error->message);
      g_clear_error (&error);
    }

  g_socket_shutdown (g_socket_connection_get_socket (conn), FALSE, TRUE, &error);
  if (error)
    {
      g_printerr ("shutdown: %s\n", error->message);
      g_clear_error (&error);
    }

  input = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  for (;;)
    {
      ret = g_input_stream_read (input, buf, sizeof (buf), NULL, &error);
      if (error)
        {
          g_printerr ("read: %s\n", error->message);
          g_clear_error (&error);
          break;
        }
      g_assert (ret >= 0);
      g_print ("%.*s", (int)ret, buf);
      if (ret == 0)
        break;
    }

  g_object_unref (conn);
  g_object_unref (client);
  g_main_loop_quit (loop);
  return NULL;
}

int
main (int argc,
      char *argv[])
{
  gint ret = 1;
  CockpitWebServer *server = NULL;
  GOptionContext *context;
  CockpitHandlerData data;
  GTlsCertificate *certificate = NULL;
  GError *local_error = NULL;
  GError **error = &local_error;
  gchar **roots = NULL;
  gchar *cert_path = NULL;
  GMainLoop *loop = NULL;
  gchar *login_html = NULL;
  gchar *login_po_html = NULL;
  CockpitPipe *pipe = NULL;
  GThread *thread = NULL;
  int outfd = -1;

  signal (SIGPIPE, SIG_IGN);
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
  g_setenv ("GIO_USE_PROXY_RESOLVER", "dummy", TRUE);
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  /* Any interaction with a krb5 ccache should be explicit */
  g_setenv ("KRB5CCNAME", "FILE:/dev/null", TRUE);

  g_setenv ("G_TLS_GNUTLS_PRIORITY", "SECURE128:%LATEST_RECORD_VERSION:-VERS-SSL3.0:-VERS-TLS1.0", FALSE);

  g_type_init ();

  memset (&data, 0, sizeof (data));

  context = g_option_context_new (NULL);
  g_option_context_add_main_entries (context, cmd_entries, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    {
      goto out;
    }

  if (opt_version)
    {
      print_version ();
      ret = 0;
      goto out;
    }

  /*
   * This process talks on stdin/stdout. However lots of stuff wants to write
   * to stdout, such as g_debug, and uses fd 1 to do that. Reroute fd 1 so that
   * it goes to stderr, and use another fd for stdout.
   */
  outfd = dup (1);
  if (outfd < 0 || dup2 (2, 1) < 1)
    {
      g_printerr ("ws couldn't redirect stdout to stderr");
      if (outfd > -1)
        close (outfd);
      goto out;
    }

  cockpit_set_journal_logging (NULL, !isatty (2));

  if (opt_request || opt_local_session || opt_no_tls)
    {
      /* no certificate */
    }
  else
    {
      cert_path = cockpit_certificate_locate (FALSE, error);
      if (cert_path != NULL)
        certificate = cockpit_certificate_load (cert_path, error);
      if (certificate == NULL)
        goto out;
      g_info ("Using certificate: %s", cert_path);
    }

  loop = g_main_loop_new (NULL, FALSE);

  data.os_release = cockpit_system_load_os_release ();
  data.auth = cockpit_auth_new (opt_local_ssh);
  roots = setup_static_roots (data.os_release);

  data.branding_roots = (const gchar **)roots;
  login_html = g_strdup (DATADIR "/cockpit/static/login.html");
  data.login_html = (const gchar *)login_html;
  login_po_html = g_strdup (DATADIR "/cockpit/static/login.po.html");
  data.login_po_html = (const gchar *)login_po_html;

  server = cockpit_web_server_new (opt_address,
                                   opt_port,
                                   certificate,
                                   NULL,
                                   error);
  if (server == NULL)
    {
      g_prefix_error (error, "Error starting web server: ");
      goto out;
    }

  cockpit_web_server_set_redirect_tls (server, !cockpit_conf_bool ("WebService", "AllowUnencrypted", FALSE));

  if (cockpit_conf_string ("WebService", "UrlRoot"))
    {
      g_object_set (server, "url-root",
                    cockpit_conf_string ("WebService", "UrlRoot"),
                    NULL);
    }
  if (cockpit_web_server_get_socket_activated (server))
    g_signal_connect_swapped (data.auth, "idling", G_CALLBACK (g_main_loop_quit), loop);

  /* Ignores stuff it shouldn't handle */
  g_signal_connect (server, "handle-stream",
                    G_CALLBACK (cockpit_handler_socket), &data);

  /* External channels, ignore stuff they shouldn't handle */
  g_signal_connect (server, "handle-stream",
                    G_CALLBACK (cockpit_handler_external), &data);

  /* Don't redirect to TLS for /ping */
  g_object_set (server, "ssl-exception-prefix", "/ping", NULL);
  g_signal_connect (server, "handle-resource::/ping",
                    G_CALLBACK (cockpit_handler_ping), &data);

  /* Files that cannot be cache-forever, because of well known names */
  g_signal_connect (server, "handle-resource::/favicon.ico",
                    G_CALLBACK (cockpit_handler_root), &data);
  g_signal_connect (server, "handle-resource::/apple-touch-icon.png",
                    G_CALLBACK (cockpit_handler_root), &data);

  /* The fallback handler for everything else */
  g_signal_connect (server, "handle-resource",
                    G_CALLBACK (cockpit_handler_default), &data);

  if (opt_local_session)
    {
      struct passwd *pwd;

      if (g_str_equal (opt_local_session, "-"))
        {
          pipe = cockpit_pipe_new (opt_local_session, 0, outfd);
          outfd = -1;
        }
      else
        {
          const gchar *args[] = { opt_local_session, NULL };
          pipe = cockpit_pipe_spawn (args, NULL, NULL, COCKPIT_PIPE_FLAGS_NONE);
        }

      /* Spawn a local session as a bridge */
      pwd = getpwuid (geteuid ());
      if (!pwd)
        {
          g_printerr ("Failed to resolve current user id %u\n", geteuid ());
          goto out;
        }
      cockpit_auth_local_async (data.auth, pwd->pw_name, pipe, on_local_ready, g_object_ref (server));
      g_object_unref (pipe);
    }
  else
    {
      /* When no local bridge, start serving immediately */
      cockpit_web_server_start (server);
    }

  /* Debugging issues during testing */
#if WITH_DEBUG
  signal (SIGABRT, cockpit_test_signal_backtrace);
  signal (SIGSEGV, cockpit_test_signal_backtrace);
#endif

  if (opt_request)
    {
      cockpit_ws_session_program = BUILDDIR "/cockpit-session";
      opt_port = cockpit_web_server_get_port (server);
      thread = g_thread_new ("fuzz-client", perform_http_request, loop);
    }

  g_main_loop_run (loop);

  ret = 0;

  if (thread)
    g_thread_join (thread);

out:
  if (outfd >= 0)
    close (outfd);
  if (loop)
    g_main_loop_unref (loop);
  if (local_error)
    {
      g_printerr ("cockpit-ws: %s\n", local_error->message);
      g_error_free (local_error);
    }
  g_clear_object (&server);
  g_clear_object (&data.auth);
  if (data.os_release)
    g_hash_table_unref (data.os_release);
  g_clear_object (&certificate);
  g_free (cert_path);
  g_strfreev (roots);
  g_free (login_po_html);
  g_free (login_html);
  g_free (opt_address);
  g_free (opt_local_session);
  cockpit_conf_cleanup ();
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */
