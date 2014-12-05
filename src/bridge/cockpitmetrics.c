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

#include "cockpitmetrics.h"
#include "cockpitpcpmetrics.h"

#include "common/cockpitjson.h"

struct _CockpitMetricsPrivate {
  guint timeout;
  gint64 next;
  gint64 interval;
};

G_DEFINE_ABSTRACT_TYPE (CockpitMetrics, cockpit_metrics, COCKPIT_TYPE_CHANNEL);

static void
cockpit_metrics_init (CockpitMetrics *self)
{

}

static void
cockpit_metrics_recv (CockpitChannel *channel,
                      GBytes *message)
{
  g_warning ("received unexpected metrics1 payload");
  cockpit_channel_close (channel, "protocol-error");
}

static void
cockpit_metrics_close (CockpitChannel *channel,
                       const gchar *problem)
{
  CockpitMetrics *self = COCKPIT_METRICS (channel);

  if (self->priv->timeout)
    {
      g_source_remove (self->priv->timeout);
      self->priv->timeout = 0;
    }

  COCKPIT_CHANNEL_CLASS (cockpit_metrics_parent_class)->close (channel, problem);
}

static void
cockpit_metrics_dispose (GObject *object)
{
  CockpitMetrics *self = COCKPIT_METRICS (object);

  if (self->priv->timeout)
    {
      g_source_remove (self->priv->timeout);
      self->priv->timeout = 0;
    }

  G_OBJECT_CLASS (cockpit_metrics_parent_class)->dispose (object);
}

static void
cockpit_metrics_class_init (CockpitMetricsClass *klass)
{
  CockpitChannelClass *channel_class = COCKPIT_CHANNEL_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = cockpit_metrics_dispose;

  channel_class->recv = cockpit_metrics_recv;
  channel_class->close = cockpit_metrics_close;
}

static gboolean
on_timeout_tick (gpointer data)
{
  CockpitMetrics *self = data;
  CockpitMetricsClass *klass;
  gint64 current;

  g_source_remove (self->priv->timeout);
  self->priv->timeout = 0;

  klass = COCKPIT_METRICS_GET_CLASS (self);

  /*
   * TODO: It would be nice to only get the system time once.
   * Not sure this can be done without sacrificing accuracy.
   */

  current = g_get_monotonic_time() / 1000;

  if (self->priv->next == 0)
    self->priv->next = current;

  klass = COCKPIT_METRICS_GET_CLASS (self);
  while (current <= self->priv->next)
    {
      self->priv->next += self->priv->interval;

      if (klass->tick)
        (klass->tick) (self, current);

      /* No idea how long above tick took */
      current = g_get_monotonic_time() / 1000;
    }

  self->priv->timeout = g_timeout_add (self->priv->next - current, on_timeout_tick, self);
  return FALSE;
}

void
cockpit_metrics_metronome (CockpitMetrics *self,
                           gint64 timestamp,
                           gint64 interval)
{
  g_return_if_fail (self->priv->timeout == 0);
  g_return_if_fail (self->priv->interval > 0);

  self->priv->next = timestamp;
  self->priv->interval = interval;
  on_timeout_tick (self);
}

CockpitChannel *
cockpit_metrics_open (CockpitTransport *transport,
                      const gchar *id,
                      JsonObject *options)
{
  GType channel_type;
  const gchar *source;

    /* Source will be further validated when channel opens */
  if (!cockpit_json_get_string (options, "source", NULL, &source))
    source = NULL;

#if 0
  if (g_strcmp0 (source, "internal") == 0)
    channel_type = COCKPIT_TYPE_INTERNAL_METRICS;
  else
#endif
    channel_type = COCKPIT_TYPE_PCP_METRICS;

  return g_object_new (channel_type,
                       "transport", transport,
                       "id", id,
                       "options", options,
                       NULL);
}
