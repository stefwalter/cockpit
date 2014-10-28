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

#include "cockpitfakemanager.h"

/* ----------------------------------------------------------------------------
 * CockpitDBusIntrospector
 */

struct _CockpitDBusCache {
  GObject parent;

  GCancellable *cancellable;

  /* Construct properties */
  GDBusConnection *connection;
  gchar *name;

  /* Introspection interface cache */
  GHashTable *introspect_cache;

  /* GHashTable(path, GHashTable(interface, GHashTable(name, GVariant))) */
  GHashTable *cache;

  /* The set of watch data */
  GHashTable *watches;

  /* Compiled and gathered information */
  GHashTable *managed;
  GHashTable *watch_paths;
  GHashTable *watch_descendants;
};

typedef struct {
  GObjectClass parent_class;
} CockpitDBusIntrospectorClass;

enum {
  PROP_CONNECTION = 1,
  PROP_NAME_OWNER,
  PROP_INTROSPECT_CACHE
};

G_DEFINE_TYPE (CockpitDBusCache, cockpit_dbus_cache, G_TYPE_OBJECT);

typedef struct {
  gint refs;
  gchar *path;
  CockpitDBusWatchType type;
} WatchData;

static void
watch_data_free (gpointer data)
{
  WatchData *wd = data;
  g_free (wd->path);
  g_free (wd);
}

static guint32
watch_data_hash (gconstpointer data)
{
  WatchData *wd = data;
  return g_str_hash (wd->path) ^ g_int_hash (&wd->type);
}

static gboolean
watch_data_eqaul (gconstpointer one,
                  gconstpointer two)
{
  WatchData *w1 = one;
  WatchData *w2 = two;
  return w1->type == w2->type && g_str_equal (w1->path, w2->path);
}

static void
cockpit_dbus_cache_init (CockpitDBusCache *self)
{
  self->cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       g_free, (GDestroyNotify)g_hash_table_unref);

  self->managed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  self->watches = g_hash_table_new_full (watch_data_hash, watch_data_equal,
                                         watch_data_free, NULL);

  self->watch_paths = g_hash_table_new (g_str_hash, g_str_equal);
  self->watch_descendants = g_hash_table_new (g_str_hash, g_str_equal);

  self->cancellable = g_cancellable_new ();
}

static void
on_properties_signal (GDBusConnection *connection,
                      const gchar *sender,
                      const gchar *path,
                      const gchar *interface,
                      const gchar *member,
                      GVariant *body,
                      gpointer user_data)
{
  CockpitDBusCache *self = user_data;
  process_properties_changed (self, path, body);
}

static void
on_manager_signal (GDBusConnection *connection,
                   const gchar *sender,
                   const gchar *path,
                   const gchar *interface,
                   const gchar *member,
                   GVariant *body,
                   gpointer user_data)
{
  CockpitDBusCache *self = user_data;

  /* Note that this is an ObjectManager */
  if (!g_hash_table_lookup (self->managed, path))
    g_hash_table_add (self->managed, g_strdup (path));

  if (g_str_equal (member, "InterfacesAdded"))
    process_interfaces_added (self, body);
  else if (g_str_equal (member, "InterfacesRemoved"))
    process_interfaces_removed (self, body);
}

static void
cockpit_dbus_cache_constructed (GObject *object)
{
  CockpitDBusCache *self = COCKPIT_FAKE_MANAGER (object);
  g_return_if_fail (self->name != NULL);
  g_return_if_fail (self->connection != NULL);

  self->subscribe_props = g_dbus_connection_signal_subscribe (self->connection,
                                                              self->name,
                                                              "org.freedesktop.DBus.Properties",
                                                              "PropertiesChanged",
                                                              NULL, /* object_path */
                                                              NULL, /* arg0 */
                                                              G_DBUS_SIGNAL_FLAGS_NONE,
                                                              on_properties_signal,
                                                              self, NULL);

  self->subscribe_manager = g_dbus_connection_signal_subscribe (self->connection,
                                                                self->name,
                                                                "org.freedesktop.DBus.ObjectManager"
                                                                NULL, /* member */
                                                                NULL, /* object_path */
                                                                NULL, /* arg0 */
                                                                G_DBUS_SIGNAL_FLAGS_NONE,
                                                                on_manager_signal,
                                                                self, NULL);

  self->subscribed = TRUE;
}

static void
cockpit_dbus_cache_set_property (GObject *obj,
                                 guint prop_id,
                                 const GValue *value,
                                 GParamSpec *pspec)
{
  CockpitDBusCache *self = COCKPIT_DBUS_CACHE (obj);

  switch (prop_id)
    {
      case PROP_CONNECTION:
        self->connection = g_value_dup_object (value);
        break;
      case PROP_NAME_OWNER:
        self->name = g_value_dup_string (value);
        break;
      case PROP_INTROSPECT_CACHE:
        self->introspect_cache = g_value_dup_boxed (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
        break;
    }
}

static void
cockpit_dbus_cache_dispose (GObject *object)
{
  CockpitDbusCache *self = COCKPIT_DBUS_CACHE (object);

  g_cancellable_cancel (self->cancellable);

  if (self->subscribed)
    {
      g_dbus_connection_signal_unsubscribe (self->connection, self->subscribe_properties);
      g_dbus_connection_signal_unsubscribe (self->connection, self->subscribe_manager);
      self->subscribed = FALSE;
    }

  if (self->connection)
    {
      g_object_unref (self->connection);
      self->connection = NULL;
    }

  if (self->introspect_cache)
    {
      g_hash_table_unref (self->introspect_cache);
      self->introspect_cache = NULL;
    }

  g_hash_table_remove_all (self->watches);
  g_hash_table_remove_all (self->cache);
  g_hash_table_remove_all (self->managed);
  g_hash_table_remove_all (self->watch_paths);
  g_hash_table_remove_all (self->watch_descendants);

  G_OBJECT_CLASS (cockpit_dbus_cache_parent_class)->dispose (object);
}

static void
cockpit_dbus_cache_finalize (GObject *object)
{
  CockpitDBusCache *self = COCKPIT_DBUS_CACHE (object);

  g_free (self->name);

  g_object_unref (self->cancellable);
  g_hash_table_unref (self->watches);
  g_hash_table_unref (self->cache);
  g_hash_table_unref (self->managed);
  g_hash_table_unref (self->watch_paths);
  g_hash_table_unref (self->watch_descendants);

  G_OBJECT_CLASS (cockpit_dbus_cache_parent_class)->finalize (object);
}

static void
cockpit_dbus_cache_class_init (CockpitFakeManagerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->constructed = cockpit_fake_manager_constructed;
  gobject_class->get_property = cockpit_fake_manager_get_property;
  gobject_class->set_property = cockpit_fake_manager_set_property;
  gobject_class->dispose = cockpit_fake_manager_dispose;
  gobject_class->finalize = cockpit_fake_manager_finalize;

  sig__present = g_signal_new ("present", COCKPIT_TYPE_DBUS_CACHE, G_SIGNAL_RUN_LAST,
                               G_OFFSET (CockpitDBusCacheClass, present),
                               NULL, NULL, NULL, G_TYPE_NONE,
                               2, G_TYPE_STRING, G_TYPE_STRING);

  sig__changed = g_signal_new ("changed", COCKPIT_TYPE_DBUS_CACHE, G_SIGNAL_RUN_LAST,
                               G_OFFSET (CockpitDBusCacheClass, changed),
                               NULL, NULL, NULL, G_TYPE_NONE,
                               4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_VARIANT);

  sig__removed = g_signal_new ("removed", COCKPIT_TYPE_DBUS_CACHE, G_SIGNAL_RUN_LAST,
                               G_OFFSET (CockpitDBusCacheClass, removed),
                               NULL, NULL, NULL, G_TYPE_NONE,
                               2, G_TYPE_STRING, G_TYPE_STRING);

  g_object_class_install_property (gobject_class, PROP_CONNECTION,
       g_param_spec_object ("connection", "connection", "connection", G_TYPE_DBUS_CONNECTION,
                            G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_INTROSPECT_CACHE,
       g_param_spec_boxed ("introspect-cache", "introspect-cache", "introspect-cache", G_TYPE_HASH_TABLE,
                           G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NAME_OWNER,
       g_param_spec_string ("name-owner", "name-owner", "name-owner", NULL,
                            G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

  GSimpleAsyncResult *res;
  const gchar *path;
} IntrospectData;

static void
on_scrape_introspected (GObject *source_object,
                        GAsyncResult *result,
                        gpointer user_data)
{
  IntrospectData *ip = user_data;
  ScrapeData *sp = g_simple_async_result_get_op_res_gpointer (ip->res);
  GError *error = NULL;
  GDBusNodeInfo *node;
  gboolean expected;
  const gchar *xml;
  GVariant *retval;
  gchar *remote;

  retval = g_dbus_connection_call_finish (self->connection, result, &error);

  /* Bail fast if cancelled */

  if (retval)
    {
      g_variant_get (retval, "(&s)", &xml);
      node = g_dbus_node_info_new_for_xml (xml, &error);
      if (node)
        {
          scrape_introspect_node (self, scrape, node);
          g_dbus_node_info_unref (node);
        }
      g_variant_unref (retval);
    }

  if (error)
    {
      /*
       * Note that many DBus implementations don't return errors when
       * an unknown object path is introspected. They just return empty
       * introspect data. GDBus is one of these.
       */

      expected = FALSE;
      remote = g_dbus_error_get_remote_error (error);
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
          expected = (g_str_equal (remote, "org.freedesktop.DBus.Error.UnknownMethod") ||
                      g_str_equal (remote, "org.freedesktop.DBus.Error.UnknownObject") ||
                      g_str_equal (remote, "org.freedesktop.DBus.Error.UnknownInterface"));
          g_free (remote);
        }

      if (!expected)
        {
          g_warning ("Couldn't look up introspection data on %s at %s: %s",
                     self->bus_name, poke->object_path, error->message);
        }
      g_error_free (error);
      poke_remove_object_and_finish (self, poke);
      return;
    }
}

static void
scrape_introspect_path (GSimpleAsyncResult *res,
                        ScrapeData *scrape,
                        const gchar *path)
{
  if (!g_hash_table_lookup (scrape->introspected_paths, path))
    {
      g_dbus_connection_call (self->connection, self->bus_name, object_path,
                              "org.freedesktop.DBus.Introspectable", "Introspect",
                              NULL, G_VARIANT_TYPE ("(s)"),
                              G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, /* timeout */
                              self->cancellable, on_scrape_introspected, res);
      self->outstanding++;
      g_hash_table_add (scrape->introspected_paths, g_strdup (path));
    }
}

#if 0
void
cockpit_fake_manager_scrape (CockpitDBusIntrospector *self,
                             GList *paths,
                             GCancellable *cancellable,
                             GAsyncReadyCallback *callback,
                             gpointer user_data)
{
  GSimpleAsyncResult *res;

  g_return_if_fail (COCKPIT_IS_FAKE_MANAGER (self));
  g_return_if_fail (g_variant_is_object_path (object_path));

  poke = poke_context_start (self, object_path);

  for()
    {
      sp = g_new0 (ScrapePath, 1);
      sp->path = g_strdup (l->data);
      g_return_if_fail (g_variant_is_object_path (sp->path));

      g_dbus_connection_call (self->connection, self->bus_name, object_path,
                              "org.freedesktop.DBus.Introspectable", "Introspect",
                              NULL, G_VARIANT_TYPE ("(s)"),
                              G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, /* timeout */
                              self->cancellable, on_scrape_introspected, res);
    }

}
#endif

static GHashTable *
snapshot_string_keys (GHashTable *table)
{
  GHashTable *set;
  GHashTableIter iter;

  set = g_hash_table_new (g_str_hash, g_str_equal);
  if (table)
    {
      g_hash_table_iter_init (&iter, table);
      while (g_hash_table_iter_next (&iter, (gpointer *)key, NULL))
        g_hash_table_add (set, key);
    }

  return set;
}

static GHashTable *
ensure_properties (CockpitDBusCache *self,
                   const gchar *path,
                   const gchar *interface)
{
  GHashTable *interfaces;
  GHashTable *properties;

  interfaces = g_hash_table_lookup (self->cache, path);
  if (!interfaces)
    {
      interfaces = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, g_hash_table_unref);
      g_hash_table_replace (self->cache, g_strdup (path), interfaces);
    }

  properties = g_hash_table_lookup (interfaces, interface);
  if (!properties)
    {
      properties = g_hash_table_new_full (g_str_hash, g_str_equal,
                                          g_free, property_value_free);
      g_hash_table_replace (interfaces, g_strdup (interface), properties);
      notify_present (self, path, interface);
    }

  return properties;
}

static gboolean
process_value (CockpitDBusCache *self,
               GHashTable *properties,
               const gchar *property,
               GVariant *variant)
{
  GVariant *value;
  GVariant *change;
  gboolean ret = FALSE;

  change = g_variant_get_variant (variant);

  if (g_hash_table_lookup_extended (properties, property, &key, &value))
    {
      if (g_variant_equal (value, change))
        {
          g_variant_unref (change);
          return FALSE;
        }

      g_hash_table_steal (properties, key);
      g_hash_table_replace (properties, key, change);
      g_variant_unref (value);
    }
  else
    {
      g_hash_table_replace (properties, g_strdup (property), change);
    }

  return TRUE;
}

static void
process_property (CockpitDBusCache *self,
                  const gchar *path,
                  const gchar *interface,
                  const gchar *property,
                  GVariant *variant)
{
  GHashTable *properties;

  properties = ensure_properties (self, path, interface);
  if (process_value (self, properties, property, variant))
    notify_changed (self, path, interface, property);
}

static void
process_properties (CockpitDBusCache *self,
                    const gchar *path,
                    const gchar *interface,
                    GVariant *dict)
{
  GHashTable *properties;
  const gchar *property;
  GVariant *variant;

  properties = ensure_properties (self, path, interface);

  g_variant_iter_init (&iter, dict);
  while (g_variant_iter_loop (&iter, "{sv}", &property, &variant))
    {
      if (process_value (self, properties, property, variant))
        notify_changed (self, path, interface, property);
    }
}

static void
process_removed (CockpitDBusCache *self,
                 const gchar *path,
                 const gchar *interface)
{
  GHashTable *interfaces;
  GHashTable *properties;

  interfaces = g_hash_table_lookup (self->cache, path);
  if (!interfaces)
    return;

  properties = g_hash_table_lookup (interfaces, interface);
  if (!properties)
    return;

  g_hash_table_remove (interfaces, interface);
  notify_removed (self, path, interface);
}

static void
process_interfaces (CockpitDBusCache *self,
                    GHashTable *snapshot,
                    const gchar *path,
                    GVariant *dict)
{
  GVariant *inner;
  const gchar *interface;

  g_variant_iter_init (&iter, dict);
  while (g_variant_iter_loop (&iter, "{s@{sv}}", &interface, &inner))
    {
      if (snapshot)
        g_hash_table_remove (snapshot, interface);
      process_properties (self, path, interface, properties);
    }
}

static void
process_paths (CockpitDBusCache *self,
               GHashTable *snapshot,
               GVariant *dict)
{
  GVariant *inner;
  GHashTable *snap;
  GHashTable *table;
  const gchar *path;

  g_variant_iter_init (&iter, dict);
  while (g_variant_iter_loop (&iter, "{s@a{sa{sv}}}", &path, &inner))
    {
      snap = NULL;
      if (snapshot)
        {
          g_hash_table_remove (snapshot, path);
          snap = snapshot_string_keys (g_hash_table_lookup (self->cache, path));
        }

      process_interfaces (self, snap, path, inner);

      if (snap)
        {
          g_hash_table_iter_init (&iter, snap);
          while (g_hash_table_iter_next (&iter, (gpointer *)&key, NULL))
            process_removed (self, path, key);
        }
    }
}

static void
process_get_all (CockpitDBusCache *self,
                 GSimpleAsyncResult *scrape,
                 const gchar *path,
                 const gchar *interface,
                 GVariant *retval)
{
  /*
   * Technically, because GetAll() returns the entire set of properties
   * on an interface, it could indicate that a property had gone away.
   * But that's not really something that is supposed to happen in DBus,
   * so don't handle that case.
   */
  process_properties (path, NULL, interface, retval);

  /* Discover other paths that we may not have retrieved yet. */
  introspect_variant_paths (self, scrape, retval);
}

static void
process_get_managed_objects (CockpitDBusCache *self,
                             const gchar *manager_root,
                             GVariant *retval)
{
  /*
   * So here we handle things slightly differently than just pushing the
   * result through all the properties update mechanics. We get
   * indications of interfaces and entire paths disappearing here,
   * so we have to handle that.
   */

  GVariant *inner;
  GVariantIter i;
  const gchar *path;

  /* Snapshot everything under control of the path of the object manager */
  snapshot = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_iter_init (&i, self->cache);
  while (g_hash_table_iter_next (&i, &path, NULL))
    {
      if (is_descendant_path (path, manager_root))
        g_hash_table_add (snapshot, key);
    }

  process_paths (self, snapshot, retval);

  g_hash_table_iter_init (&i, snapshot);
  while (g_hash_table_iter_next (&i, &path, NULL))
    {
      interfaces = g_hash_table_lookup (self->cache, path);
      if (interfaces)
        {
          g_hash_table_iter_init (&j, interfaces);
          while (g_hash_table_iter_next (&j, &interface, NULL))
            process_removed (self, path, interface);
        }
    }
  g_hash_table_unref (snapshot);
}

typedef struct {
  CockpitDBusCache *cache;
  gchar *path;
  GVariant *params;
} GetData;

static void
on_get_reply (GObject *source,
              GAsyncResult *result,
              gpointer user_data)
{
  GetData *gd = user_data;
  CockpitDBusCache *self = gd->cache;
  const gchar *property;
  const gchar *interface;
  GError *error = NULL;
  GVariant *retval;

  g_variant_get (gp->params, "(&s&s)", &interface, &property);

  retval = g_dbus_connection_call (self->connection, result, &error);
  if (error)
    {
      if (!g_cancellable_is_cancelled (self->cancellable))
        {
          g_message ("%s: couldn't get property %s %s at %s", self->name,
                     interface, property, gd->path);
        }
      g_error_free (error);
    }

  if (retval)
    {
      process_get (self, path, interface, property, retval);
      g_variant_unref (retval);
    }

  g_object_unref (gd->cache);
  g_variant_unref (gd->params);
  g_free (gd->path);
  g_free (gd);
}

static void
process_properties_changed (CockpitDBusCache *self,
                            const gchar *path,
                            GVariant *body)
{
  GetProperty *gp;
  GVariantIter iter;
  const gchar *property;

  g_variant_get (body, "(&s@a{sv}@as)", &interface, &changed, &invalidated);

  process_properties (self, path, interface, changed);

  g_variant_iter_init (&iter, invalidated);
  while (g_variant_iter_loop (&iter, "&s", &property))
    {
      gd = g_new0 (GetData, 1);
      gd->cache = g_object_ref (self);
      gd->path = g_strdup (path);
      gd->params = g_variant_new ("(ss)", interface, property);
      g_variant_ref_sink (gd->params);

      g_dbus_connection_call (self->connection, self->name, gd->path,
                              "org.freedesktop.DBus.Properties", "Get",
                              gd->params, G_VARIANT_TYPE ("(v)"),
                              G_DBUS_CALL_FLAGS_NO_AUTO_START, -1,
                              self->cancellable, on_get_property, gd);
    }

  g_variant_unref (invalidated);
  g_variant_unref (changed);
}

static void
process_interfaces_added (CockpitDBusCache *self,
                          GVariant *body)
{
  GVariant *interfaces;
  const gchar *path;

  g_variant_get (body, "(&s@a{sa{sv}})", &path, &interfaces);
  process_interfaces (self, NULL, path, interfaces);
  g_variant_unref (interfaces);
}

static void
process_interfaces_removed (CockpitDBusCache *self,
                            GVariant *body)
{
  GVariant *array;
  const gchar *path;

  g_variant_get (body, "(&s@as)", &path, &array);

  g_variant_iter_init (&iter, array);
  while (g_variant_iter_loop (&iter, "&s", &interface))
    process_removed (self, path, interface);

  g_variant_unref (array);
}

typedef struct {
  CockpitDBusCache *cache;
  gchar *path;
  GSimpleAsyncResult *scrape;
} IntrospectData;

static void
on_introspect_reply (GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
  IntrospectData *id = user_data;
  CockpitDBusCache *self = id->cache;
  GError *error = NULL;
  GVariant *retval;

  retval = g_dbus_connection_call_finish (self->connection, result, &error);
  if (error)
    {
      if (!g_cancellable_is_cancelled (self->cancellable))
        g_message ("%s: couldn't introspect %s", self->name, id->path);
      g_error_free (error);
    }

  if (retval)
    {
      process_introspect (self, id->scrape, id->path, retval);
      g_variant_unref (retval);
    }

  if (id->scrape)
    {
      scrape_decrement (id->scrape);
      g_object_unref (id->scrape);
    }
  g_object_unref (id->cache);
  g_free (id->path);
  g_free (id);
}

static void
process_introspect_children (CockpitDBusCache *self,
                             const gchar *parent_path,
                             GDBusNodeInfo *node)
{
  GHashTable *snapshot;

  /* Snapshot all direct children of path */
  snapshot = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_iter_init (&i, self->cache);
  while (g_hash_table_iter_next (&i, &path, NULL))
    {
      if (is_descendant_path (path, parent_path))
        g_hash_table_add (snapshot, path);
    }

  /* Poke any additional child nodes discovered */
  for (i = 0; node->nodes && node->nodes[i]; i++)
    {
      child = node->nodes[i];

      /* If the child has no path then it's useless */
      if (!child->path)
        continue;

      /* Figure out an object path for this node */
      if (g_str_has_prefix (child->path, "/"))
        child_path = g_strdup (child->path);
      else if (g_str_equal (parent_path, "/"))
        child_path = g_strdup_printf ("/%s", child->path);
      else
        child_path = g_strdup_printf ("%s/%s", parent_path, child->path);

      /* Remove everything in the snapshot related to child */
      g_hash_table_iter_init (&iter, snapshot);
      while (g_hash_table_iter_next (&iter, &path, NULL))
        {
          if (g_str_equal (path, child_path) ||
              is_descendant_path (path, child_path))
            g_hash_table_iter_remove (iter);
        }

      /* Inline child interfaces are rare but possible */
      if (child->interfaces || child->interfaces[0])
        {
          process_introspect_node (self, NULL, child_path, child);
          g_free (child_path);
        }

      /* If we have no knowledge of this child, then introspect it */
      else if (!is_managed_object (self, child_path) &&
               !is_path_known (self, child_path))
        {
          id = g_new0 (IntrospectData, 1);
          id->cache = g_object_ref (self);
          id->path = child_path;

          g_dbus_connection_call (self->connection, self->name, id->path,
                                  "org.freedesktop.DBus.Introspectable", "Introspect",
                                  g_variant_new ("()"), G_VARIANT_TYPE ("(s)"),
                                  G_DBUS_CALL_FLAGS_NO_AUTO_START, -1,
                                  self->cancellable, on_introspect_reply, id);
        }
      else
        {
          g_free (child_path);
        }
    }

  /* Anything remaining in snapshot stays */
  g_hash_table_iter_init (&iter, snapshot);
  while (g_hash_table_iter_next (&iter, &path, NULL))
    process_remove_path (self, path);
  g_hash_table_unref (snapshot);
}

typedef struct {
  CockpitDBusCache *cache;
  gchar *path;
  GVariant *params;
  GSimpleAsyncResult *scrape;
} GetAllData;

static void
on_get_all_reply (GObject *source,
                  GAsyncResult *result,
                  gpointer user_data)
{
  GetAllData *gad = user_data;
  CockpitDBusCache *self = gad->cache;
  const gchar *interface;
  GError *error = NULL;
  GVariant *retval;

  g_variant_get (gad->params, "(&s)", &interface);

  retval = g_dbus_connection_call_finish (self->connection, result, &error);
  if (error)
    {
      if (!g_cancellable_is_cancelled (self->cancellable))
        {
          g_message ("%s: couldn't get all properties of %s at %s", self->name,
                     interface, gad->path);
        }
      g_error_free (error);
    }

  if (retval)
    {
      process_get_all (self, gad->scrape, path, interface, retval);
      g_variant_unref (retval);
    }

  if (gad->scrape)
    {
      scrape_decrement (scrape);
      g_object_unref (id->scrape);
    }
  g_object_unref (gad->cache);
  g_variant_unref (gad->params);
  g_free (gad->path);
  g_free (gad);
}

static void
process_introspect_node (CockpitDBusCache *self,
                         GSimpleAsyncResult *scrape,
                         const gchar *path,
                         GDBusNodeInfo *node)
{
  GDBusInterfaceInfo *iface;
  GDBusNodeInfo *child;
  gchar *child_path;
  GetAllData *gad;
  gint i;

  snapshot = snapshot_string_keys (g_hash_table_lookup (self->cache, path));

  for (i = 0; node->interfaces && node->interfaces[i] != NULL; i++)
    {
      iface = node->interfaces[i];
      if (!iface->name)
        {
          g_warning ("Received interface from %s at %s without name", sp->name, sp->path);
          continue;
        }

      /* Cache this interface for later use elsewhere */
      if (self->introspect_cache)
        {
          if (!g_hash_table_lookup (self->introspect_cache, iface->name))
            {
              g_hash_table_replace (self->introspect_cache, iface->name,
                                    g_dbus_interface_info_ref (iface));
            }
        }

      /* Skip these interfaces */
      if (g_str_has_prefix (iface->name, "org.freedesktop.DBus."))
        continue;

      g_hash_table_remove (snapshot, iface->name);
      ensure_properties (self, path, iface->name);

      gad = g_new0 (GetAllData, 1);
      gad->cache = g_object_ref (self);
      gad->path = g_strdup (path);
      gad->params = g_variant_new ("(s)", iface->name);
      g_variant_ref_sink (gad->params);
      if (scrape)
        {
          scrape_increment (scrape);
          gad->scrape = g_object_ref (scrape);
        }
      g_dbus_connection_call (self->connection, self->name, path,
                              "org.freedesktop.DBus.Properties", "GetAll",
                              gp->params, G_VARIANT_TYPE ("(a{sv})"),
                              G_DBUS_CALL_FLAGS_NO_AUTO_START, -1,
                              self->cancellable, on_get_all_reply, gad);
    }

  /* Remove any interfaces not seen */
  g_hash_table_iter_init (&iter, snapshot);
  while (g_hash_table_iter_next (&iter, &interface, NULL))
    process_removed (self, path, interface);
  g_hash_table_destroy (snapshot);

  process_introspect_children (self, path, node);
}

static void
process_introspect (CockpitDBusCache *self,
                    const gchar *path,
                    GVariant *body)
{
  GDBusNodeInfo *node;
  const gchar *xml;

  g_variant_get (retval, "(&s)", &xml);
  node = g_dbus_node_info_new_for_xml (xml, &error);
  if (node)
    {
      process_introspect_node (self, scrape, path, node);
      g_dbus_node_info_unref (node);
    }
}

static void
introspect_path (CockpitDBusCache *self,
                 GSimpleAsyncResult *scrape,
                 const gchar *path)
{
  IntrospectData *id;

  /* Are we interested in this at all? */
  if (!g_hash_table_lookup (self->watch_paths) &&
      !lookup_descendant (self->watch_descendants, path))
    return;

  /* A managed object never gets introspected */
  if (!lookup_descendant (self->managed, path))
    return;

  id = g_new0 (IntrospectData, 1);
  id->cache = g_object_ref (self);
  id->path = g_strdup (path);
  if (scrape)
    {
      scrape_increment (scrape);
      id->scrape = g_object_ref (scrape);
    }

  g_dbus_connection_call (self->connection, self->name, id->path,
                          "org.freedesktop.DBus.Introspectable", "Introspect",
                          g_variant_new ("()"), G_VARIANT_TYPE ("(s)"),
                          G_DBUS_CALL_FLAGS_NO_AUTO_START, -1,
                          self->cancellable, on_introspect_reply, id);
}

static void
introspect_variant_paths (CockpitDBusCache *self,
                          GSimpleAsyncResult *scrape,
                          GVariant *data)
{
  if (g_variant_is_of_type (data, G_VARIANT_TYPE_OBJECT_PATH))
    {
      path = g_variant_get_string (variant, NULL);
      if (!g_str_equal (path, "/") && !is_path_known (self, path))
        introspect_path (self, scrape, path);
    }
  else if (g_variant_is_container (variant))
    {
      g_variant_iter_init (&iter, variant);
      while ((child = g_variant_iter_next_value (&iter)) != NULL)
        {
          introspect_variant_paths (self, scrape, child);
          g_variant_unref (child);
        }
    }
}

typedef struct {
  CockpitDBusCache *cache;
  gchar *path;
} GetManagedObjectsData;

static void
on_get_managed_objects_reply (GObject *source,
                              GAsyncResult *result,
                              gpointer user_data)
{
  GetManagedObjectsData *gmod = user_data;
  CockpitDBusCache *self = gmod->cache;
  GError *error = NULL;
  GVariant *retval;

  retval = g_dbus_connection_call_finish (self->connection, result, &error);
  if (error)
    {
      g_message ("%s: couldn't get managed objects at %s", self->name, gmod->path);
      g_error_free (error);
    }

  if (retval)
    {
      process_get_managed_objects (self, gmod->path, retval);
      g_variant_unref (retval);
    }

  g_object_unref (gmod->cache);
  g_free (gmod->path);
  g_free (gmod);
}

static void
recompile_watches (CockpitDBusCache *self)
{
  GHashTableIter iter;
  WatchData *wd;

  /*
   * Recompile all the lookup tables ... note that we assume that
   * once something is an ObjectManager tree it always will be
   * even if a watch gets removed.
   *
   * Also note that ObjectManager doesn't cover the top of the tree
   * only the sub nodes.
   */

  g_hash_table_remove_all (self->watch_paths);
  g_hash_table_remove_all (self->watch_descendants);

  g_hash_table_iter_init (&iter, self->watches);
  while (g_hash_table_iter_next (&iter, (gpointer *)&wd, NULL))
    {
      switch (wd->type)
        {
        case COCKPIT_DBUS_WATCH_PATH:
          g_hash_table_add (self->watch_paths, wd->path);
          break;
        case COCKPIT_DBUS_WATCH_OBJECT_MANAGER:
          g_hash_table_add (self->managed, wd->path);
          g_hash_table_add (self->watch_desdendants, wd->path);
          break;
        case COCKPIT_DBUS_WATCH_PATH_NAMESPACE:
          g_hash_table_add (self->watch_paths, wd->path);
          g_hash_table_add (self->watch_descendants, wd->path);
          break;
        default:
          g_warn_if_reached ();
          break;
        }
    }
}

void
cockpit_dbus_cache_watch (CockpitDBusCache *self,
                          const gchar *path,
                          CockpitDBusWatchType type)
{
  WatchData *wd;
  WatchData *prev;

  wd = g_new0 (WatchData, 1);
  wd->refs = 1;
  wd->path = g_strdup (path);
  wd->type = type;

  prev = g_hash_table_lookup (self->watches, wd);
  if (prev)
    {
      prev->refs++;
      watch_data_free (wd);
    }
  else
    {
      g_hash_table_add (self->watches, wd);
      recompile_watches (self);
    }

  switch (type)
    {
    case COCKPIT_DBUS_WATCH_PATH:
    case COCKPIT_DBUS_WATCH_PATH_NAMESPACE:
      introspect_path (self, NULL, wd->path);
      break;

    case COCKPIT_DBUS_WATCH_OBJECT_MANAGER:
      gmod = g_new (GetManagedObjects, 1);
      gmod->path = g_strdup (wd->path);
      gmod->cache = g_object_ref (self);

      dbus_connection_call (self->connection, self->name, gmod->path,
                            "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                            g_variant_new ("()"), G_VARIANT_TYPE ("a{s@a{sa{sv}}}"),
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, /* timeout */
                            self->cancellable, on_get_managed_objects_reply, gmod);
      break;

    default:
      g_return_if_reached ();
      break;
    }
}

gboolean
cockpit_dbus_cache_unwatch (CockpitDBusCache *self,
                            const gchar *path,
                            CockpitDBusWatchType type)
{
  WatchData wd = { 1, path, type };
  WatchData *prev;

  prev = g_hash_table_lookup (self->watches, &wd);
  if (!prev)
    return FALSE;

  prev->refs--;
  if (prev->refs == 0)
    {
      g_hash_table_remove (self->watches, prev);
      recompile_watches (self);
    }

  return TRUE;
}

void
cockpit_dbus_cache_scrape (CockpitDBusCache *self,
                           GVariant *data,
                           GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data)
{
  GSimpleAsyncResult *scrape;
  gint *outstanding;

  scrape = g_simple_async_result_new (G_OBJECT (self),
                                      callback, user_data,
                                      cockpit_dbus_cache_scrape);
  outstanding = g_new0 (gint, 1);
  g_simple_async_result_set_op_res_gpointer (scrape, outstanding, g_free);
  scrape_add_outstanding (scrape);

  introspect_variant_paths (self, scrape, data);

  scrape_remove_outstanding (scrape);
  g_object_unref (scrape);
}

gboolean
cockpit_dbus_cache_scrape_finish (CockpitDBusCache *self,
                                  GAsyncResult *result,
                                  GError **error)
{
  g_return_val_if_fail (g_simple_async_result_is_valid (result, G_OBJECT (self),
                                                        cockpit_dbus_cache_scrape), FALSE);

  return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}


#if 0
typedef struct {
  CockpitDBusCache *cache;
  MessageHandler handler;
  GVariant *body;
} MessageData;

static void
message_data_free (gpointer data)
{
  MessageData *message = data;
  g_object_unref (data->cache);
  g_variant_unref (data->body);
  g_free (data);
}

static gboolean
on_message_process (gpointer user_data)
{
  SignalData *sd = user_data;
  CockpitDBusCache *self = sd->cache;
  gboolean matched = TRUE;
  GHashTableIter iter;
  WatchData *wd;

  path = g_dbus_message_get_path (sd->message);
  if (!path)
    return FALSE;

  /* This is a possible future optimization point, once usage patterns are clear */
  g_hash_table_iter_init (&iter, self->watches);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&wd))
    {
      if (wd->path_namespace)
        {
          if (g_str_has_prefix (path, wd->path))
            {
              matched = TRUE;
              break;
            }
        }
      else
        {
          if (g_str_equal (path, wd->path))
            {
              matched = TRUE;
              break;
            }
        }
    }

  if (matched)
    (sd->handler) (sd->cache, sd->message);
  return FALSE;
}

static void
invoke_message_handler (CockpitDBusCache *self,
                        MessageHandler handler,
                        GVariant *body)
{
  SignalData *message = g_new0 (MessageData, 1);
  sd->cache = g_object_ref (self);
  sd->generation = g_atomic_int_inc (&self->generation);
  sd->body = g_variant_ref (body);
  g_main_context_invoke_full (self->context, G_PRIORITY_DEFAULT,
                              on_message_process, message,
                              message_data_free);
}

static GDBusMessage *
on_message_filter (GDBusConnection *connection,
                   GDBusMessage *message,
                   gboolean incoming,
                   gpointer user_data)
{
  CockpitDBusCache *self = user_data;
  const gchar *member;
  const gchar *sender;
  const gchar *interface;
  GDBusMessageType type;

  if (incoming)
    {
      sender = g_dbus_message_get_sender (message);
      if (!sender || g_strcmp0 (sender, self->name) == 0)
        {
          type = g_dbus_message_get_message_type (message);

          if (type == G_DBUS_MESSAGE_TYPE_SIGNAL)
            {
              interface = g_dbus_message_get_interface (message);
              member = g_dbus_message_get_member (message);

              if (g_str_equal (interface, "org.freedesktop.DBus.Properties") &&
                  g_str_equal (member, "PropertiesChanged"))
                {
                  g_object_set_qdata (message, quark__generation, next_generation (self));
                  invoke_message_handler (self, process_properties_changed, message);
                }
              else if (g_str_equal (interface, "org.freedesktop.DBus.ObjectManager"))
                {
                  if (g_str_equal (member, "InterfacesAdded"))
                    {
                      g_object_set_qdata (message, quark__generation, next_generation (self));
                      invoke_message_handler (self, process_interfaces_added, message);
                    }
                  else if (g_str_equal (member, "InterfacesRemoved"))
                    {
                      g_object_set_qdata (message, quark__generation, next_generation (self));
                      invoke_message_handler (self, process_interfaces_removed, message);
                    }
                }
            }
          else if (type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN)
            {
              g_object_set_qdata (message, quark__generation, next_generation (self));
            }
        }
    }

  return message;
}

static void
process_incoming_signal (CockpitDBusJson *self,
                         GDBusMessage *message)
{
  const gchar *sender;
  const gchar *path;
  const gchar *interface;

  /* Must match sender we're talking to */
  sender = g_dbus_message_get_sender (message);
  if (sender && g_strcmp0 (sender, self->name) != 0 &&
      g_strcmp0 (sender, self->name_owner) != 0)
    return;

  path = g_dbus_message_get_interface (path);
  interface = g_dbus_message_get_interface (message);

  process_watches (self, path, interface, message);
  process_rules (self, path, interface, message);
}

#endif
