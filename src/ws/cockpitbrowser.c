/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2018 Red Hat, Inc.
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

#include "cockpitbrowser.h"

#include <string.h>
#include <unistd.h>

/*
 * This launches Firefox (eventually we can add support for using
 * the browser default) and connects it to the running cockpit-ws
 * instance.
 *
 * We generate a token in order to work with cockpit-token as well.
 * See token.c for the other side of the authenticator that uses this.
 */

static GVariant *
build_firefox_arguments (const gchar *url)
{
  const gchar *argv[] = { "firefox", "--new-window", url };
  guint32 offsets[G_N_ELEMENTS (argv) + 1];
  GByteArray *encoded;
  GVariant *result;
  guint32 offset;
  const char *cwd;
  guint8 byte;
  gint i;

  encoded = g_byte_array_sized_new (1024);
  cwd = "/";

  /* The first number here */
  offsets[0] = G_N_ELEMENTS (argv);
  offset = (G_N_ELEMENTS (argv) + 1) * sizeof (guint32);

  /* The working directory */
  /* Each measure all the arguments */
  offset += strlen (cwd) + 1;
  for (i = 0; i < G_N_ELEMENTS (argv); i++)
    {
      offsets[i + 1] = offset;
      offset += strlen (argv[i]) + 1;
    }

  /* Now we apply all the offsets backwards to the front */
  for (i = 0; i < G_N_ELEMENTS (offsets); i++)
    {
      /* All numbers are encoded little-endian */
      offset = offsets[i];
      byte = (offset & 0xff000000) >> 24;
      g_byte_array_append (encoded, &byte, 1);
      byte = (offset & 0x00ff0000) >> 16;
      g_byte_array_append (encoded, &byte, 1);
      byte = (offset & 0x0000ff00) >> 8;
      g_byte_array_append (encoded, &byte, 1);
      byte = (offset & 0x00000000) >> 0;
      g_byte_array_append (encoded, &byte, 1);
    }

  /* Now every string */
  g_byte_array_append (encoded, (const guint8 *)cwd, strlen (cwd));
  for (i = 0; i < G_N_ELEMENTS (argv); i++)
    g_byte_array_append (encoded, (const guint8 *)argv[i], strlen (argv[i]));

  result = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTESTRING, encoded->data, encoded->len, 1);
  g_byte_array_unref (encoded);

  return result;
}

gboolean
cockpit_browser_launch (const gchar *address,
                        gint port)
{
  gchar *url;

  if (!address)
    address = "localhost";
  if (!port)
    port = 9090;


  url = g_strdup_printf ("http://user:%s@%s:%u", token, address, (guint)port);
  parameters = g_variant_new ("(ay)", build_firefox_arguments (url));
  g_free (url);

  g_dbus_connection_call_sync (connection, "org.mozilla.firefox.ZGVmYXVsdA__",
                               "/org/mozilla/firefox/Remote", "org.mozilla.firefox",
                               "OpenURL", parameters, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                               &error);

}
