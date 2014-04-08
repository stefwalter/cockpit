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

#ifndef __COCKPIT_INTROSPECTOR_H__
#define __COCKPIT_INTROSPECTOR_H__

#include <gio/gio.h>

G_BEGIN_DECLS

GDBusNodeInfo *      cockpit_introspector_call_introspect    (GDBusConnection *connection,
                                                              const gchar *bus_name,
                                                              const gchar *object_path,
                                                              GError **error);

GDBusInterfaceInfo * cockpit_introspector_lookup_interface   (GDBusConnection *connection,
                                                              const gchar *bus_name,
                                                              const gchar *object_path,
                                                              const gchar *interface_name,
                                                              GError **error);

void                 cockpit_introspector_store_interface    (const gchar *bus_name,
                                                              GDBusInterfaceInfo *info);

G_END_DECLS

#endif /* __COCKPIT_INTROSPECTOR_H__ */
