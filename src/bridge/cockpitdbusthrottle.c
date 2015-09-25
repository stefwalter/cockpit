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

#include "cockpitdbusthrottle.h"

/**
 * Since we're a bridge to DBus, we are often in the situation where the
 * dbus-daemon thinks we have too many messages outstanding. This code
 * lets us throttle the sending of messages to placate the picky system
 * bus.
 */

typedef struct {
    gint maximum;
    gint outstanding;
    GQueue *queue;
} CockpitDBusThrottle;

static void
cockpit_dbus_throttle_free (gpointer data)
{
  CockpitDBusThrottle *throttle = data;
  if (throttle)
    {
      g_queue_free_full (throttle->queue, g_object_unref);
      g_free (throttle);
    }
}

static GDBusMessage *
on_throttle_dbus_message (GDBusConnection *connection,
                          GDBusMessage *message,
                          gboolean incoming,
                          gpointer user_data)
{
  CockpitDBusThrottle *throttle;
  GDBusMessageType type;
  GDBusMessage *message;

  type = g_dbus_message_get_message_type (message);

  if (incoming)
    {
      if (type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN ||
          type == G_DBUS_MESSAGE_TYPE_ERROR)
        {
          if (throttle->outstanding == 0)
            {
              g_warning ("dbus connection throttle out of sync, turning off");
              throttle->maximum = G_MAXINT;
            }
          else
            {
              throttle->outstanding -= 1;
            }
        }
    }
  else
    {
      throttle->outstanding = xxxx
    }
}

static guint
cockpit_dbus_throttle_connection (GDBusConnection *connection,
                                  gint maximum_outstanding)
{
  CockpitDBusThrottle *throttle;

  g_return_if_fail (G_IS_DBUS_CONNECTION (connection));
  g_return_if_fail (maximum_outstanding > 0);

  throttle = g_new0 (CockpitDBusThrottle, 1);
  throttle->maximum = maximum_outstanding;
  throttle->queue = g_queue_new ();

  return g_dbus_connection_add_filter (connection, on_throttle_dbus_message,
                                       throttle, cockpit_dbus_throttle_free);
}
