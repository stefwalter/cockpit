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

#include <pcp/pmapi.h>

/**
 * CockpitPcpMetrics:
 *
 * A #CockpitMetrics channel that pulls data from PCP
 */

#define COCKPIT_PCP_METRICS(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_PCP_METRICS, CockpitPcpMetrics))

typedef struct {
  CockpitMetrics parent;
  const gchar *name;
  int context;
  int numpmid;
  pmID *pmidlist;
  gchar **metrics;
  gchar **instances;
  gint64 timestamp;
  int delta;
} CockpitPcpMetrics;

typedef struct {
  CockpitMetricsClass parent_class;
} CockpitPcpMetricsClass;

G_DEFINE_TYPE (CockpitPcpMetrics, cockpit_pcp_metrics, COCKPIT_TYPE_METRICS);

static void
cockpit_pcp_metrics_init (CockpitPcpMetrics *self)
{
  self->context = -1;
}

#if 0
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
#endif

static void
cockpit_pcp_metrics_prepare (CockpitChannel *channel)
{
  CockpitPcpMetrics *self = COCKPIT_PCP_METRICS (channel);
  const gchar *problem = "protocol-error";
  const gchar *source;
  JsonObject *options;
  gboolean ret = FALSE;
  const char *name;
  gint64 interval;
  gint64 value;
  int type;
  int rc;

  COCKPIT_CHANNEL_CLASS (cockpit_pcp_metrics_parent_class)->prepare (channel);

  options = cockpit_channel_get_options (channel);

  /* "source" option */
  if (!cockpit_json_get_string (options, "source", NULL, &source))
    {
      g_warning ("invalid \"source\" option for metrics channel");
      goto out;
    }
  else if (!source)
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
  else if (g_str_equal (source, "pmcd"))
    {
      type = PM_CONTEXT_HOST;
      name = "127.0.0.1:44321";
    }
  else
    {
      g_message ("unsupported \"source\" option specified for metrics: %s", source);
      problem = "not-supported";
      goto out;
    }

  self->name = source;
  self->context = pmNewContext(type, name);
  if (self->context < 0)
    {
      g_warning ("%s: couldn't create PCP context: %s", self->name, pmErrStr (self->context));
      problem = "internal-error";
      goto out;
    }

  /* "metrics" option */
  self->numpmid = 0;
  if (!cockpit_json_get_strv (options, "metrics", NULL, &self->metrics))
    {
      g_warning ("%s: invalid \"metrics\" option was specified", self->name);
      goto out;
    }
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

  /* "interval" option */
  if (!cockpit_json_get_int (options, "interval", 1000, &interval))
    {
      g_warning ("%s: invalid \"interval\" option", self->name);
      goto out;
    }
  else if (interval <= 0 || interval > G_MAXINT)
    {
      g_warning ("%s: invalid \"interval\" value: %" G_GINT64_FORMAT, self->name, interval);
      goto out;
    }

  /* "timestamp" option */
  if (!cockpit_json_get_int (options, "timestamp", 0, &timestamp))
    {
      g_warning ("%s: invalid \"timestamp\" option");
      goto out;
    }
  if (timestamp < 0 || timestamp / 1000 > G_MAXLONG)
    {
      g_warning ("%s: invalid \"timestamp\" value: %" G_GINT64_FORMAT, self->name, timestamp);
      goto out;
    }

  /* "instances" option */
  if (!cockpit_json_get_strv (options, "instances", NULL, &self->instances))
    {
      g_warning ("%s: invalid \"instances\" option");
      goto out;
    }

  /* "limit" option */
  if (!cockpit_json_get_int (options, "instances", -1, &self->limit))
    {
      g_warning ("%s: invalid \"instances\" option");
      goto out;
    }
  else if (self->limit < -1)
    {
      g_warning ("%s: invalid \"limit\" option value");
      goto out;
    }

  problem = NULL;
  if (type == PM_CONTEXT_ARCHIVE)
    perform_load (self, timestamp, interval);

  else
    cockpit_metrics_metronome (COCKPIT_METRICS (self), timestamp, interval);
  cockpit_channel_ready (channel);

out:
  if (problem)
    cockpit_channel_close (channel, problem);
}

static void
cockpit_pcp_metrics_finalize (GObject *object)
{
  CockpitPcpMetrics *self = COCKPIT_PCP_METRICS (object);

  if (self->context >= 0)
    pmDestroyContext (self->context);
  g_strfreev (self->metrics);
  g_strfreev (self->instances);
  g_free (self->pmidlist);

  G_OBJECT_CLASS (cockpit_pcp_metrics_parent_class)->finalize (object);
}

static void
cockpit_pcp_metrics_class_init (CockpitPcpMetricsClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  CockpitChannelClass *channel_class = COCKPIT_CHANNEL_CLASS (klass);

  gobject_class->finalize = cockpit_pcp_metrics_finalize;
  channel_class->prepare = cockpit_pcp_metrics_prepare;
}
