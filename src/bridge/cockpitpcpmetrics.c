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

#include "cockpitpcpmetrics.h"

/**
 * CockpitPcpMetrics:
 *
 * A #CockpitMetrics channel that pulls data from PCP
 */

#define COCKPIT_PCP_METRICS(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_PCP_METRICS, CockpitPcpMetrics))

typedef struct {
  CockpitMetrics parent;
  gchar *name;
  int context;
  int numpmid;
  pmID *pmidlist;
} CockpitPcpMetrics;

typedef struct {
  CockpitMetricsClass parent_class;
} CockpitPcpMetricslClass;

G_DEFINE_TYPE (CockpitPcpMetrics, cockpit_pcp_metrics, COCKPIT_TYPE_CHANNEL);

static void
cockpit_pcp_metrics_recv (CockpitChannel *channel,
                          GBytes *message)
{
  g_warning ("%s: received unexpected metrics1 payload: %s", name);
  cockpit_channel_close (channel, "protocol-error");
}

static void
cockpit_pcp_metrics_init (CockpitPcpMetrics *self)
{

}

static gboolean
parse_options (CockpitChannel *channel,
               int *context_type,
               const char **context_name,
               const gchar ***metrics,
               int *numpmid,
               pmID **pmidlist,
               int *interval,
               struct timeval *start,
               struct timeval *end,
               const gchar ***instances)
{
  const gchar *problem = "protocol-error";
  const gchar *source;
  gboolean ret = FALSE;

  source = cockpit_channel_get_option (channel, "source");
  if (!source)
    {
      g_warning ("no \"source\" option specified for metrics channel");
      goto out;
    }
  else if (g_str_has_prefix (source, "/"))
    {
      *context_type = PM_CONTEXT_ARCHIVE;
      *context_name = source;
    }
  else if (g_str_equal (source, "system"))
    {
      *context_type = PM_CONTEXT_LOCAL;
      *context_name = NULL;
    }
  else
    {
      g_warning ("unsupported \"source\" option specified for metrics: %s", source);
      goto out;
    }

  *metrics = 

}

static gboolean
on_timeout_fetch (gpointer data)
{
  CockpitPcpMetrics *self = data;
  pmResult *result;

  /* Always schedule a new wait */
  self->wait = 0;

  if (pumUseContext (self->context) < 0)
    g_return_val_if_reached (FALSE);

  rc = pmFetch (self->numpmid, self->pmidlist, &result);
  if (rc < 0)
    {
      g_warning ("%s: couldn't fetch metrics: %s", self->name, pmStrErr (rc));
      return FALSE;
    }

  send_meta_if_necessary (self, result);

  when = from_timeval (&result->timestamp);

  if (self->count == 0)
    self->epoch = when;

  do
    {
      send_result_metrics (self, result);

      if (self->last && self->last != result)
        {
          pmFreeResult (self->last);
          self->last = result;
        }
    }
  
    {
      
  if ()
    {
      
    }
  


  /* First time we're called? */
  if (self->epoch == 0)
    {
      
    


  return FALSE;
}

static void
cockpit_pcp_metrics_constructed (GObject *object)
{
  CockpitPcpMetrics *self = COCKPIT_PCP_METRICS (object);
  CockpitChannel *channel = COCKPIT_CHANNEL (object);
  const gchar *problem = "protocol-error";
  const gchar *source;
  gboolean ret = FALSE;
  const char *name;
  int type;

  G_OBJECT_CLASS (cockpit_pcp_metrics_parent_class)->constructed (object);

  source = cockpit_channel_get_option (channel, "source");
  self->name = source;

  if (!source)
    {
      g_warning ("no \"source\" option specified for metrics channel");
      goto out;
    }
  else if (g_str_has_prefix (source, "/"))
    {
      type = PM_CONTEXT_ARCHIVE;
      name = source;
    }
  else if (g_str_equal (source, "system"))
    {
      type = PM_CONTEXT_LOCAL;
      name = NULL;
    }
  else
    {
      g_warning ("unsupported \"source\" option specified for metrics: %s", source);
      goto out;
    }

  self->context = pmContextNew (type, name);
  if (self->context < 0)
    {
      g_warning ("%s: couldn't create PCP context: %s", self->name, pmErrStr (self->context));
      problem = "internal-error";
      goto out;
    }

  self->numpmid = 0;
  self->metrics = cockpit_channel_get_strv_option (channel, "metrics");
  if (self->metrics)
    self->numpmid = g_strv_length (self->metrics);
  if (self->numpmid == 0)
    {
      g_warning ("%s: no \"metrics\" were specified", self->name);
      goto out;
    }

  self->pmidlist = g_new0 (pmID, self->numpmid);
  rc = pmLookupName (self->numpmid, self->metrics, self->pmidlist);
  if (rc < 0)
    {
      g_warning ("%s: invalid \"metrics\" names: %s", self->name, pmErrStr (self->context));
      goto out;
    }

  value = cockpit_channel_get_int_option (channel, "interval");
  if (value == 0)
    value = 1000;
  if (value < 0 || value > G_MAXINT)
    {
      g_warning ("%s: invalid \"interval\" value: %" G_GINT64_FORMAT, self->name, value);
      goto out;
    }

  self->interval = (int)value;

  value = cockpit_channel_get_int_option (channel, "begin");
  xxxx end xxxx;

  self->instances = cockpit_channel_get_strv_option (channel, "instances");

  ret = TRUE;

  if (type == PM_CONTEXT_LOCAL)
    {
      self->epoch = g_get_monotonic_time ();
      self->wait = g_idle_add_full (G_PRIORITY_HIGH_IDLE, on_timeout_fetch, self, NULL);
    }
  else
    {
      self->wait = g_idle_add_full (G_PRIORITY_IDLE, on_timeout_load, self, NULL);
    }

out:
  if (ret)
    cockpit_channel_ready (channel);
  else
    cockpit_channel_close (channel, problem);
}

static void
cockpit_echo_channel_class_init (CockpitEchoChannelClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  CockpitChannelClass *channel_class = COCKPIT_CHANNEL_CLASS (klass);

  gobject_class->constructed = cockpit_echo_channel_constructed;
  channel_class->recv = cockpit_echo_channel_recv;
}
