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

#include "cockpitdbussession.h"
#include "cockpitunixfd.h"

#include <sys/prctl.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
setup_dbus_daemon (gpointer addrfd)
{
  g_unsetenv ("G_DEBUG");
  cockpit_unix_fd_close_all (3, GPOINTER_TO_INT (addrfd));
}

static void
setup_dbus_testing (gpointer addrfd)
{
  prctl (PR_SET_PDEATHSIG, SIGINT);
  setup_dbus_daemon (addrfd);
}

GPid
cockpit_dbus_session_launch (const gchar *test_config,
                             gchar **out_address)
{
  GSpawnChildSetupFunc setup;
  gchar *config_arg = NULL;
  GError *error = NULL;
  GString *address = NULL;
  gchar *line;
  gsize len;
  gssize ret;
  GPid pid = 0;
  gchar *print_address = NULL;
  int addrfd[2] = { -1, -1 };
  GSpawnFlags flags;

  gchar *dbus_argv[] = {
      "dbus-daemon",
      "--print-address=X",
      "--session",
      "--nofork",
      NULL
  };

  if (pipe (addrfd))
    {
      g_warning ("pipe failed to allocate fds: %m");
      goto out;
    }

  print_address = g_strdup_printf ("--print-address=%d", addrfd[1]);
  dbus_argv[1] = print_address;

  /* The DBus daemon produces useless messages on stderr mixed in */
  flags = G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_SEARCH_PATH;

  if (test_config)
    {
      setup = setup_dbus_testing;
      config_arg = g_strdup_printf ("--config-file=%s", test_config);
      dbus_argv[2] = config_arg;
    }
  else
    {
      if (!g_getenv ("G_MESSAGES_DEBUG"))
        flags |= G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_STDOUT_TO_DEV_NULL;
      setup = setup_dbus_daemon;
    }

  g_spawn_async_with_pipes (NULL, dbus_argv, NULL, flags,
                            setup, GINT_TO_POINTER (addrfd[1]),
                            &pid, NULL, NULL, NULL, &error);

  close (addrfd[1]);

  if (error != NULL)
    {
      g_warning ("couldn't start %s: %s", dbus_argv[0], error->message);
      g_error_free (error);
      pid = 0;
      goto out;
    }

  g_debug ("launched %s", dbus_argv[0]);

  address = g_string_new ("");
  for (;;)
    {
      len = address->len;
      g_string_set_size (address, len + 256);
      ret = read (addrfd[0], address->str + len, 256);
      if (ret < 0)
        {
          g_string_set_size (address, len);
          if (errno != EAGAIN && errno != EINTR)
            {
              g_warning ("couldn't read address from dbus-daemon: %s", g_strerror (errno));
              goto out;
            }
        }
      else if (ret == 0)
        {
          g_string_set_size (address, len);
          break;
        }
      else
        {
          g_string_set_size (address, len + ret);
          line = strchr (address->str, '\n');
          if (line != NULL)
            {
              *line = '\0';
              break;
            }
        }
    }

  if (address->str[0] == '\0')
    {
      g_warning ("dbus-daemon didn't send us a dbus address");
    }
  else
    {
      g_debug ("session bus address: %s", address->str);
      g_setenv ("DBUS_SESSION_BUS_ADDRESS", address->str, TRUE);
      if (out_address)
        {
          *out_address = g_string_free (address, FALSE);
          address = NULL;
        }
    }

out:
  if (addrfd[0] >= 0)
    close (addrfd[0]);
  g_free (print_address);
  g_free (config_arg);
  if (address)
    g_string_free (address, TRUE);
  return pid;
}
