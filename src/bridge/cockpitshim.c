/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2017 Red Hat, Inc.
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

#include "common/cockpitjson.h"
#include "common/cockpitpipetransport.h"
#include "common/cockpittransport.h"

#include "cockpitrouter.h"
#include "cockpitshim.h"
#include "cockpitchannel.h"

#include <sys/wait.h>
#include <string.h>

gint cockpit_shim_bridge_timeout = 30;

static gchar *pcp_argv[] = {
    PACKAGE_LIBEXEC_DIR "/cockpit-pcp", NULL
  };

/* For now this is global */
gchar **cockpit_shim_argv = pcp_argv;

/**
 * CockpitShim:
 *
 * A channel which relays its messages to another cockpit-bridge
 * or helper on stdio.
 */

#define COCKPIT_SHIM(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_SHIM, CockpitShim))
#define COCKPIT_IS_SHIM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), COCKPIT_TYPE_SHIM))

typedef struct {
  CockpitChannel parent;

  CockpitTransport *peer;
  gulong peer_recv_sig;
  gulong peer_closed_sig;
  gulong peer_control_sig;

  gboolean sent_close;
} CockpitShim;

typedef struct {
  CockpitChannelClass parent_class;
} CockpitShimClass;

G_DEFINE_TYPE (CockpitShim, cockpit_shim, COCKPIT_TYPE_CHANNEL);

static void
cockpit_shim_init (CockpitShim *self)
{

}

static void
cockpit_shim_prepare (CockpitChannel *channel)
{
  CockpitShim *self = COCKPIT_SHIM (channel);
  GBytes *bytes = NULL;

  if (!self->peer)
    {
      cockpit_channel_close (channel, "not-supported");
      return;
    }

  bytes = cockpit_json_write_bytes (cockpit_channel_get_options (channel));
  cockpit_transport_send (self->peer, NULL, bytes);
  g_bytes_unref (bytes);
}

static void
cockpit_shim_recv (CockpitChannel *channel,
                   GBytes *message)
{
  CockpitShim *self = COCKPIT_SHIM (channel);
  const gchar *id = cockpit_channel_get_id (channel);

  if (self->peer)
    cockpit_transport_send (self->peer, id, message);
}

static gboolean
cockpit_shim_control (CockpitChannel *channel,
                      const gchar *command,
                      JsonObject *message)
{
  CockpitShim *self = COCKPIT_SHIM (channel);
  GBytes *bytes = NULL;

  if (self->peer)
    {
      bytes = cockpit_json_write_bytes (message);
      cockpit_transport_send (self->peer, NULL, bytes);
    }

  g_bytes_unref (bytes);
  return TRUE;
}

static void
cockpit_shim_close (CockpitChannel *channel,
                    const gchar *problem)
{
  CockpitShim *self = COCKPIT_SHIM (channel);
  JsonObject *object;
  GBytes *bytes;
  const gchar *id;

  if (self->peer)
    {
      if (!self->sent_close)
        {
          id = cockpit_channel_get_id (COCKPIT_CHANNEL (self));

          g_debug ("sending close for shim channel: %s: %s", id, problem);

          object = json_object_new ();
          json_object_set_string_member (object, "command", "close");
          json_object_set_string_member (object, "channel", id);
          json_object_set_string_member (object, "problem", problem);

          bytes = cockpit_json_write_bytes (object);
          json_object_unref (object);

          cockpit_transport_send (self->peer, NULL, bytes);
          g_bytes_unref (bytes);
        }

      cockpit_shim_release (channel, self->peer);
      g_object_unref (self->peer);
      self->peer = NULL;
    }

  COCKPIT_CHANNEL_CLASS (cockpit_shim_parent_class)->close (channel, problem);
}


static void
cockpit_shim_constructed (GObject *object)
{
  CockpitShim *self = COCKPIT_SHIM (object);

  G_OBJECT_CLASS (cockpit_shim_parent_class)->constructed (object);

  self->peer = cockpit_shim_ensure (COCKPIT_CHANNEL (self), cockpit_shim_argv);
}

static void
cockpit_shim_class_init (CockpitShimClass *klass)
{
  CockpitChannelClass *channel_class = COCKPIT_CHANNEL_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->constructed = cockpit_shim_constructed;

  channel_class->prepare = cockpit_shim_prepare;
  channel_class->recv = cockpit_shim_recv;
  channel_class->control = cockpit_shim_control;
  channel_class->close = cockpit_shim_close;
}

/* --------------------------------------------------------------------------------
 * External Bridge related
 */

static GBytes *last_init;
static GHashTable *bridges_by_id;
static GHashTable *bridges_by_peer;

typedef struct {
  CockpitTransport *peer;
  GHashTable *channels;
  gchar *id;
  gulong recv_sig;
  gulong control_sig;
  gulong closed_sig;
  guint timeout;
  gboolean got_init;
} ExternalBridge;


static void
external_bridge_free (gpointer data)
{
  ExternalBridge *bridge = data;
  if (bridge->timeout)
    g_source_remove (bridge->timeout);

  g_signal_handler_disconnect (bridge->peer, bridge->closed_sig);
  g_signal_handler_disconnect (bridge->peer, bridge->recv_sig);
  g_signal_handler_disconnect (bridge->peer, bridge->control_sig);

  g_hash_table_destroy (bridge->channels);
  g_object_unref (bridge->peer);
  g_free (bridge->id);
  g_free (bridge);
}

static void
on_peer_closed (CockpitTransport *transport,
                const gchar *problem,
                gpointer user_data)
{
  ExternalBridge *bridge = user_data;
  GList *l, *channels;

  g_debug ("closed external bridge: %s", bridge->id);

  xxxx if it closed without an init ... then ask channel to replay xxxx

  if (!problem)
    problem = "disconnected";

  channels = g_hash_table_get_values (bridge->channels);
  for (l = channels; l != NULL; l = g_list_next (l))
    cockpit_channel_close (l->data, problem);
  g_list_free (channels);

  g_hash_table_remove (bridges_by_id, bridge->id);

  /* This owns the bridge */
  g_hash_table_remove (bridges_by_peer, bridge->peer);
}

static gboolean
on_peer_recv (CockpitTransport *transport,
              const gchar *channel_id,
              GBytes *payload,
              gpointer user_data)
{
  ExternalBridge *bridge = user_data;
  CockpitChannel *channel = NULL;

  if (channel_id)
    channel = g_hash_table_lookup (bridge->channels, channel_id);
  if (channel)
    {
      cockpit_channel_send (channel, payload, TRUE);
      return TRUE;
    }

  return FALSE;
}

static gboolean
on_peer_control (CockpitTransport *transport,
                 const char *command,
                 const gchar *channel_id,
                 JsonObject *options,
                 GBytes *payload,
                 gpointer user_data)
{
  ExternalBridge *bridge = user_data;
  CockpitChannel *channel = NULL;

  if (g_str_equal (command, "init"))
    {
      bridge->got_init = TRUE;
      return TRUE;
    }

  if (!channel_id && options)
    cockpit_json_get_string (options, "channel", NULL, &channel_id);

  if (channel_id)
    channel = g_hash_table_lookup (bridge->channels, channel_id);
  if (channel)
    {
      if (g_str_equal (command, "close"))
        {
          cockpit_channel_close_options (channel, options);
          cockpit_channel_close (channel, NULL); /* problem set above */
        }
      else if (g_str_equal (command, "ready"))
        {
          cockpit_channel_ready (channel, options);
        }
      else
        {
          cockpit_channel_control (channel, command, options);
        }
    }

  return TRUE;
}

static gboolean
on_peer_timeout (gpointer user_data)
{
  ExternalBridge *bridge = user_data;

  bridge->timeout = 0;
  if (g_hash_table_size (bridge->channels) == 0)
    {
      /*
       * This should cause the transport to immediately be closed
       * and that will trigger removal from the main router lookup tables.
       */
      g_debug ("bridge: (%s) timed out without channels", bridge->id);
      cockpit_transport_close (bridge->peer, "timeout");
    }

  return FALSE;
}

CockpitTransport *
cockpit_shim_ensure (CockpitChannel *channel,
                     gchar **argv)
{
  ExternalBridge *bridge;
  CockpitPipe *pipe;

  gchar *id = NULL;

  if (!bridges_by_peer)
    {
      /* Owns the ExternalBridge */
      bridges_by_peer = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, external_bridge_free);
      bridges_by_id = g_hash_table_new (g_str_hash, g_str_equal);
    }

  id = g_strjoinv ("|", (gchar **) argv);

  bridge = g_hash_table_lookup (bridges_by_id, id);
  if (bridge)
    {
      if (bridge->timeout)
        {
          g_source_remove (bridge->timeout);
          bridge->timeout = 0;
        }

      g_free (id);
    }
  else
    {
      pipe = cockpit_pipe_spawn ((const gchar**) argv, NULL, NULL, 0);

      bridge = g_new0 (ExternalBridge, 1);
      bridge->peer = cockpit_pipe_transport_new (pipe);
      bridge->id = id;
      bridge->channels = g_hash_table_new (g_str_hash, g_str_equal);
      bridge->recv_sig = g_signal_connect (bridge->peer, "recv", G_CALLBACK (on_peer_recv), bridge);
      bridge->control_sig = g_signal_connect (bridge->peer, "control", G_CALLBACK (on_peer_control), bridge);
      bridge->closed_sig = g_signal_connect (bridge->peer, "closed", G_CALLBACK (on_peer_closed), bridge);

      g_hash_table_insert (bridges_by_peer, bridge->peer, bridge);
      g_hash_table_insert (bridges_by_id, bridge->id, bridge);

      cockpit_transport_send (bridge->peer, NULL, last_init);

      g_object_unref (pipe);
  }

  /* The channel owns the id, and deregisters itself */
  if (channel)
    g_hash_table_replace (bridge->channels, (gpointer)cockpit_channel_get_id (channel), channel);

  return g_object_ref (bridge->peer);
}

void
cockpit_shim_release (CockpitChannel *channel,
                      CockpitTransport *peer)
{
  ExternalBridge *bridge;
  const gchar *channel_id;

  bridge = g_hash_table_lookup (bridges_by_peer, peer);
  g_return_if_fail (bridge != NULL);

  channel_id = cockpit_channel_get_id (channel);
  if (!g_hash_table_remove (bridge->channels, channel_id))
    g_return_if_reached ();

  if (g_hash_table_size (bridge->channels) == 0)
    {
      /* Close sessions that are no longer in use after N seconds * of them being that way. */
      g_debug ("removed last channel or bridge %s", bridge->id);
      bridge->timeout = g_timeout_add_seconds (cockpit_shim_bridge_timeout, on_peer_timeout, bridge);
    }
}

void
cockpit_shim_reset (GBytes *init)
{
  ExternalBridge *bridge = NULL;
  GHashTableIter iter;

  if (init)
    g_bytes_ref (init);
  if (last_init)
    g_bytes_unref (last_init);
  last_init = init;

  if (bridges_by_peer)
    {
      g_hash_table_iter_init (&iter, bridges_by_peer);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer *)&bridge))
        cockpit_transport_close (bridge->peer, NULL);

      g_hash_table_destroy (bridges_by_id);
      bridges_by_id = NULL;
      g_hash_table_destroy (bridges_by_peer);
      bridges_by_peer = NULL;
    }
}
