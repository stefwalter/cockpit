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

#include "cockpitdbusutil.h"

gboolean
cockpit_dbus_error_matches_unknown (GError *error)
{
  gboolean ret = FALSE;

  if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
    return TRUE;

#if !GLIB_CHECK_VERSION(2,42,0)
  return g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE) ||
        g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT);
#else

  gchar *remote = g_dbus_error_get_remote_error (error);
  if (remote)
    {
      /*
       * DBus used to only have the UnknownMethod error. It didn't have
       * specific errors for UnknownObject and UnknownInterface. So we're
       * pretty liberal on what we treat as an expected error here.
       *
       * HACK: GDBus also doesn't understand the newer error codes :S
       *
       * https://bugzilla.gnome.org/show_bug.cgi?id=727900
       */
      ret = (g_str_equal (remote, "org.freedesktop.DBus.Error.UnknownMethod") ||
             g_str_equal (remote, "org.freedesktop.DBus.Error.UnknownObject") ||
             g_str_equal (remote, "org.freedesktop.DBus.Error.UnknownInterface"));
      g_free (remote);
    }

  return ret;
#endif
}
