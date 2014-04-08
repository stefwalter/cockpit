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

#include "cockpitintrospector.h"

static GHashTable *cache = NULL;

GDBusNodeInfo *
cockpit_introspector_call_introspect (GDBusConnection *connection,
                                      const gchar *bus_name,
                                      const gchar *object_path,
                                      GError **error)
{
  GDBusNodeInfo *node;
  GVariant *val;
  const gchar *xml;

  val = g_dbus_connection_call_sync (connection,
                                     bus_name,
                                     object_path,
                                     "org.freedesktop.DBus.Introspectable",
                                     "Introspect",
                                     NULL,
                                     G_VARIANT_TYPE ("(s)"),
                                     G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                     -1, /* timeout */
                                     NULL, /* GCancellable */
                                     error);
  if (val == NULL)
    return NULL;

  g_variant_get (val, "(&s)", &xml);
  node = g_dbus_node_info_new_for_xml (xml, error);

  g_variant_unref (val);
  return node;
}

GDBusInterfaceInfo *
cockpit_introspector_lookup_interface (GDBusConnection *connection,
                                       const gchar *bus_name,
                                       const gchar *object_path,
                                       const gchar *interface_name,
                                       GError **error)
{
  GDBusNodeInfo *node = NULL;
  GDBusInterfaceInfo *ret = NULL;
  int i;

  /*
   * TODO: This method is syncronous. It's sorta passable right now,
   * but will definitely need to change once an agent supports more
   * than one channel.
   */

  g_return_val_if_fail (g_dbus_is_interface_name (interface_name), NULL);
  g_return_val_if_fail (g_dbus_is_name (bus_name), NULL);
  g_return_val_if_fail (g_variant_is_object_path (object_path), NULL);

  if (cache)
    {
      ret = g_hash_table_lookup (cache, interface_name);
      if (ret != NULL)
        return ret;
    }

  node = cockpit_introspector_call_introspect (connection, bus_name,
                                               object_path, error);
  if (node == NULL)
    goto out;

  for (i = 0; node->interfaces && node->interfaces[i]; i++)
    {
      if (g_strcmp0 (node->interfaces[i]->name, interface_name) == 0)
        ret = node->interfaces[i];
      cockpit_introspector_store_interface (bus_name, node->interfaces[i]);
    }

  if (ret == NULL)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_FAILED,
                   "No info about interface %s in introspection data object at path %s owned by %s",
                   interface_name, object_path, bus_name);
      goto out;
    }

out:
  if (node != NULL)
    g_dbus_node_info_unref (node);
  return ret;
}

void
cockpit_introspector_store_interface (const gchar *bus_name,
                                      GDBusInterfaceInfo *info)
{
  g_return_if_fail (g_dbus_is_name (bus_name));
  g_return_if_fail (info != NULL);

  /*
   * TODO: The @bus_name is ignored for now. We assume that the same
   * interface is identical across multiple implementations. This will
   * probably need to be changed in the future.
   *
   * Think corner cases like services being upgraded, restarting, etc...
   */

  if (cache == NULL)
    cache = g_hash_table_new (g_str_hash, g_str_equal);

  /* We can't just replace stuff in the cache, as it may be in use */
  if (!g_hash_table_lookup (cache, info->name))
    g_hash_table_insert (cache, info->name, g_dbus_interface_info_ref (info));
}
