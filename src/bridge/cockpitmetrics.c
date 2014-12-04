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

G_DEFINE_ABSTRACT_TYPE (CockpitMetrics, cockpit_metrics, COCKPIT_TYPE_CHANNEL);

static void
cockpit_metrics_init (CockpitMetrics *self)
{

}

static void
cockpit_pcp_metrics_recv (CockpitChannel *channel,
                          GBytes *message)
{
  g_warning ("received unexpected metrics1 payload");
  cockpit_channel_close (channel, "protocol-error");
}

static void
cockpit_metrics_prepare (CockpitChannel *channel)
{
  COCKPIT_CHANNEL_CLASS (cockpit_metrics_parent_class)->prepare (channel);
}

static void
cockpit_metrics_class_init (CockpitMetricsClass *klass)
{
  CockpitChannelClass *channel_class = COCKPIT_CHANNEL_CLASS (klass);

  channel_class->prepare = cockpit_echo_channel_prepare;
  channel_class->recv = cockpit_echo_channel_recv;
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
