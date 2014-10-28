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

#ifndef __COCKPIT_DBUS_INTROSPECTOR_H
#define __COCKPIT_DBUS_INTROSPECTOR_H

/*
 * This is a fake GDBusObjectManager implementation which does not depend
 * on a server side implementation of org.freedesktop.DBus.ObjectManager.
 *
 * It's not perfect.
 *
 * Use cockpit_fake_manager_poke() to make it look up an object. It'll
 * automatically follow trees of properties and things that it understands.
 *
 * Use cockpit_fake_manager_scrape() to pass it a GVariant that possibly has
 * one or more object paths (nested anywhere) which it should look up and
 * be aware of.
 */

#include <gio/gio.h>

G_BEGIN_DECLS

#define COCKPIT_TYPE_DBUS_INTROSPECTOR         (cockpit_fake_manager_get_type ())
#define COCKPIT_DBUS_INTROSPECTOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_DBUS_INTROSPECTOR, CockpitDBusIntrospector))
#define COCKPIT_IS_DBUS_INTROSPECTOR(k)        (G_TYPE_CHECK_INSTANCE_TYPE ((k), COCKPIT_TYPE_DBUS_INTROSPECTOR))

typedef struct _CockpitDBusCache CockpitDBusCache;
typedef struct _CockpitDBusCacheClass CockpitDBusCacheClass;

struct _CockpitDBusCacheClass {
  GObjectClass parent;

  void    (* present)  (CockpitDBusCache *self,
                        const gchar *path,
                        const gchar *interface);

  void    (* changed)  (CockpitDBusCache *self,
                        const gchar *path,
                        const gchar *interface,
                        const gchar *property,
                        GVariant *value);

  void    (* removed)  (CockpitDBusCache *self,
                        const gchar *path,
                        const gchar *interface);
};

GType                      cockpit_dbus_introspector_get_type          (void) G_GNUC_CONST;

CockpitDBusIntrospector *  cockpit_dbus_introspector_new               (GDBusConnection *connection,
                                                                        const gchar *bus_name);

GDBusInterfaceInfo *       cockpit_dbus_introspector_lookup            (CockpitDBusIntrospector *self,
                                                                        const gchar *interface);

void                       cockpit_dbus_introspector_introspect        (CockpitDBusIntrospector *self,
                                                                        const gchar *path,
                                                                        const gchar *interface,
                                                                        GCancellable *cancellable,
                                                                        GAsyncReadyCallback *callback,
                                                                        gpointer user_data);

GDBusInterfaceInfo *       cockpit_dbus_introspector_introspect_finish (CockpitDBusIntrospector *self,
                                                                        GAsyncResult *result,
                                                                        GError **error);

void                       cockpit_dbus_introspector_scrape            (CockpitDBusIntrospector *self,
                                                                        GList *paths,
                                                                        GCancellable *cancellable,
                                                                        GAsyncReadyCallback *callback,
                                                                        gpointer user_data)

GVariant *                 cockpit_dbus_introspector_scrape_finish     (CockpitDBusIntrospector *self,
                                                                        GAsyncResult *result,
                                                                        GError **error);


void
cockpit_dbus_cache_watch (CockpitDBusCache *self,
                          const gchar *path,
                          CockpitDBusWatchType type);

gboolean
cockpit_dbus_cache_unwatch (CockpitDBusCache *self,
                            const gchar *path,
                            CockpitDBusWatchType type);

void
cockpit_dbus_cache_scrape (CockpitDBusCache *self,
                           GVariant *data,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data);

gboolean
cockpit_dbus_cache_scrape_finish (CockpitDBusCache *self,
                                  GAsyncResult *result,
                                  GError **error);

G_END_DECLS

#endif /* __COCKPIT_DBUS_INTROSPECTOR_H
