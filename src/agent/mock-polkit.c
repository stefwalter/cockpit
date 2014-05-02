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

#include <polkit/polkit.h>

#include <glib.h>
#include <gio/gio.h>

#include <err.h>

static gboolean
check_dbus_action (const gchar *sender,
                   const gchar *action_id)
{
  PolkitAuthorizationResult *result;
  PolkitAuthority *authority;
  PolkitSubject *subject;
  GError *error = NULL;
  gboolean ret;

  g_return_val_if_fail (sender != NULL, FALSE);
  g_return_val_if_fail (action_id != NULL, FALSE);

  authority = polkit_authority_get_sync (NULL, &error);
  if (authority == NULL)
    errx (3, "failure to get polkit authority: %s", error->message);

  subject = polkit_system_bus_name_new (sender);
  result = polkit_authority_check_authorization_sync (authority, subject, action_id, NULL,
                POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION, NULL, &error);

  g_object_unref (authority);
  g_object_unref (subject);

  /* failed */
  if (result == NULL)
    errx (3, "couldn't check polkit authorization%s%s",
          error ? ": " : "", error ? error->message : "");

  ret = polkit_authorization_result_get_is_authorized (result);
  g_object_unref (result);
  return ret;
}

int
main (int argc,
      char *argv[0])
{
  if (argc != 3)
    {
      g_printerr ("usage: mock-polkit :sender action\n");
      return 2;
    }
  if (!g_dbus_is_unique_name (argv[1]))
    errx (2, "invalid dbus name: %s", argv[1]);

  if (check_dbus_action (argv[1], argv[2]))
    {
      g_print ("authorized\n");
      return 0;
    }
  else
    {
      g_print ("not authorized\n");
      return 1;
    }
}
