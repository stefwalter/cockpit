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
  pmDesc *pmdescs;
  gchar **metrics;
  gchar **instances;
  gint64 interval;
  gint64 limit;
  guint idler;

  GHashTable *filter;

  /* The previous samples sent */
  pmResult *last;
} CockpitPcpMetrics;

typedef struct {
  CockpitMetricsClass parent_class;
} CockpitPcpMetricsClass;

typedef struct {
  pmID pmid;
  int inst;
} FilterPair;

G_DEFINE_TYPE (CockpitPcpMetrics, cockpit_pcp_metrics, COCKPIT_TYPE_METRICS);

static void
cockpit_pcp_metrics_init (CockpitPcpMetrics *self)
{
  self->context = -1;
}

static guint
filter_pair_hash (gconstpointer v)
{
  const FilterPair *pair = v;
  return g_int_hash (&pair->pmid) ^ g_int_hash (&pair->inst);
}

static gboolean
filter_pair_equal (gconstpointer v1,
                   gconstpointer v2)
{
  const FilterPair *p1 = v1;
  const FilterPair *p2 = v2;
  return p1->pmid == p2->pmid && p1->inst == p2->inst;
}

static gboolean
result_meta_equal (pmResult *r1,
                   pmResult *r2)
{
  pmValueSet *vs1;
  pmValueSet *vs2;
  pmValue *v1;
  pmValue *v2;
  int i, j;

  /* PCP guarantees that the result ids are same as requested */
  for (i = 0; i < r1->numpmid; i++)
    {
      vs1 = r1->vset[i];
      vs2 = r2->vset[i];

      g_assert (vs1 && vs2);

      if (vs1->numval != vs2->numval ||
          vs1->valfmt != vs2->valfmt)
        return FALSE;

      for (j = 0; j < vs1->numval; j++)
        {
          v1 = vs1->vlist + i;
          v2 = vs2->vlist + i;

          if (v1->inst != v2->inst)
            return FALSE;
        }
    }

  return TRUE;
}

static void
send_object (CockpitPcpMetrics *self,
             JsonObject *object)
{
  CockpitChannel *channel = (CockpitChannel *)self;
  GBytes *bytes;

  bytes = cockpit_json_write_bytes (object);
  cockpit_channel_send (channel, bytes, TRUE);
  g_bytes_unref (bytes);
}

static void
send_array (CockpitPcpMetrics *self,
            JsonArray *array)
{
  CockpitChannel *channel = (CockpitChannel *)self;
  GBytes *bytes;
  JsonNode *node;
  gsize length;
  gchar *ret;

  node = json_node_new (JSON_NODE_ARRAY);
  json_node_set_array (node, array);
  ret = cockpit_json_write (node, &length);
  json_node_free (node);

  bytes = g_bytes_new_take (ret, length);
  cockpit_channel_send (channel, bytes, TRUE);
  g_bytes_unref (bytes);
}

static JsonObject *
build_meta (CockpitPcpMetrics *self,
            pmResult *result)
{
  JsonArray *instances;
  JsonArray *array;
  JsonObject *root;
  pmValueSet *vs;
  FilterPair *pair;
  gint64 timestamp;
  char *instance;
  int i, j, k;
  int rc;

  timestamp = (result->timestamp.tv_sec * 1000) +
              (result->timestamp.tv_usec / 1000);

  root = json_object_new ();
  json_object_set_int_member (root, "timestamp", timestamp);
  json_object_set_int_member (root, "interval", self->interval);

  if (self->filter)
    g_hash_table_remove_all (self->filter);

  instances = json_array_new ();
  for (i = 0; i < result->numpmid; i++)
    {
      vs = result->vset[i];

      /* TODO: Figure out how instanced values with one instance differ
       * from non-instanced values. Is there a meaningful difference? */

      /* When negative numval is an error code ... we don't care */
      if (vs->numval <= 0)
        {
          json_array_add_null_element (instances);
        }
      else
        {
          array = json_array_new ();

          for (j = 0; j < vs->numval; j++)
            {
              /* PCP guarantees that the result is in the same order as requested */
              rc = pmNameInDom (self->pmdescs[i].indom, vs->vlist[j].inst, &instance);
              if (rc != 0)
                {
                  g_warning ("%s: instance name lookup failed: %s", self->name, pmErrStr (rc));
                  instance = strdup ("");
                }

              json_array_add_string_element (array, instance);

              /* Let the filtering know about the instance id */
              if (self->filter)
                {
                  for (k = 0; self->instances[k] != NULL; k++)
                    {
                      if (g_str_equal (self->instances[k], instance))
                        {
                          pair = g_new0 (FilterPair, 1);
                          pair->pmid = vs->pmid;
                          pair->inst = vs->vlist[j].inst;
                          g_hash_table_add (self->filter, pair);
                        }
                    }
                }
              free (instance);
            }
          json_array_add_array_element (instances, array);
        }
    }

  json_object_set_array_member (root, "instances", instances);
  return root;
}

static JsonObject *
build_meta_if_necessary (CockpitPcpMetrics *self,
                         pmResult *result)
{
  if (self->last)
    {
      /*
       * If we've already sent the first meta message, then only send
       * another when the set of instances in the results change.
       */

      if (result_meta_equal (self->last, result))
        return NULL;
    }

  return build_meta (self, result);
}

static JsonNode *
build_sample (int valfmt,
              pmDesc *desc,
              pmValue *value)
{
  pmAtomValue atom;
  JsonNode *node;

  if (desc->type == PM_TYPE_AGGREGATE || desc->type == PM_TYPE_EVENT)
    return json_node_new (JSON_NODE_NULL);

  pmExtractValue (valfmt, value, desc->type, &atom, desc->type);

  node = json_node_new (JSON_NODE_VALUE);
  switch (desc->type) {
    case PM_TYPE_32:
      json_node_set_int (node, atom.l);
      break;
    case PM_TYPE_U32:
      json_node_set_int (node, atom.ul);
      break;
    case PM_TYPE_64:
      json_node_set_int (node, atom.ll);
      break;
    case PM_TYPE_U64:
      json_node_set_int (node, atom.ull);
      break;
    case PM_TYPE_FLOAT:
      json_node_set_double (node, atom.f);
      break;
    case PM_TYPE_DOUBLE:
      json_node_set_double (node, atom.d);
      break;
    case PM_TYPE_STRING:
      json_node_set_string (node, atom.cp);
      break;
    case PM_TYPE_AGGREGATE:
    case PM_TYPE_EVENT:
      g_assert_not_reached ();
      break;
  };

  return node;
}

static JsonArray *
build_samples (CockpitPcpMetrics *self,
               pmResult *result)
{
  JsonArray *samples;
  JsonArray *array;
  pmValueSet *vs;
  int i, j;

  samples = json_array_new ();

  /* TODO: Implement interframe compression, both of nulls and
   * trailing values. */

  for (i = 0; i < result->numpmid; i++)
    {
      vs = result->vset[i];

      /* TODO: Figure out how instanced values with one instance differ
       * from non-instanced values. Is there a meaningful difference? */

      /* When negative numval is an error code ... we don't care */
      if (vs->numval <= 0)
        {
          json_array_add_null_element (samples);
        }
      else if (vs->numval == 1)
        {
          /* TODO: This code path should only be taken for non-instanced values */
          json_array_add_element (samples, build_sample (vs->valfmt, self->pmdescs + i, vs->vlist));
        }
      else
        {
          array = json_array_new ();

          for (j = 0; j < vs->numval; j++)
            {
              if (self->filter)
                {
                  FilterPair key = { vs->pmid, vs->vlist[j].inst };
                  if (!g_hash_table_lookup (self->filter, &key))
                    continue;
                }

              json_array_add_element (samples, build_sample (vs->valfmt, self->pmdescs + i, vs->vlist + j));
            }

          json_array_add_array_element (samples, array);
        }
    }

  return samples;
}

static void
cockpit_pcp_metrics_tick (CockpitMetrics *metrics,
                          gint64 timestamp)
{
  CockpitPcpMetrics *self = (CockpitPcpMetrics *)metrics;
  JsonArray *message;
  JsonObject *meta;
  pmResult *result;
  int rc;

  if (pmUseContext (self->context) < 0)
    g_return_if_reached ();

  rc = pmFetch (self->numpmid, self->pmidlist, &result);
  if (rc < 0)
    {
      g_warning ("%s: couldn't fetch metrics: %s", self->name, pmErrStr (rc));
      cockpit_channel_close (COCKPIT_CHANNEL (self), "internal-error");
      return;
    }

  meta = build_meta_if_necessary (self, result);
  if (meta)
    {
      send_object (self, meta);
      json_object_unref (meta);
    }

  /* Send one set of samples */
  message = json_array_new ();
  json_array_add_array_element (message, build_samples (self, result));
  send_array (self, message);
  json_array_unref (message);

  if (self->last)
    {
      pmFreeResult (self->last);
      self->last = result;
    }

  /* Sent enough samples? */
  self->limit--;
  if (self->limit <= 0)
    cockpit_channel_close (COCKPIT_CHANNEL (self), NULL);
}

static gboolean
on_idle_batch (gpointer user_data)
{
  const int archive_batch = 60;
  CockpitPcpMetrics *self = user_data;
  JsonArray *message = NULL;
  const gchar *problem;
  JsonObject *meta;
  pmResult *result;
  gint i;
  int rc;

  if (pmUseContext (self->context) < 0)
    g_return_if_reached ();

  for (i = 0; i < archive_batch; i++)
    {
      /* Sent enough samples? */
      self->limit--;
      if (self->limit <= 0)
        rc = PM_ERR_EOL;
      else
        rc = pmFetch (self->numpmid, self->pmidlist, &result);
      if (rc < 0)
        {
          if (rc == PM_ERR_EOL)
            {
              problem = NULL;
              if (message)
                send_array (self, message);
            }
          else
            {
              g_message ("%s: couldn't read from archive: %s", self->name, pmErrStr (self->context));
              problem = "internal-error";
            }
          cockpit_channel_close (COCKPIT_CHANNEL (self), problem);
          if (message)
            json_array_unref (message);
          return FALSE;
        }

      meta = build_meta_if_necessary (self, result);
      if (meta)
        {
          send_array (self, message);
          json_array_unref (message);
          message = NULL;

          send_object (self, meta);
          json_object_unref (meta);
        }

      if (message == NULL)
          message = json_array_new ();
      json_array_add_array_element (message, build_samples (self, result));

      if (self->last)
        {
          pmFreeResult (self->last);
          self->last = result;
        }
    }

  if (message)
    {
      send_array (self, message);
      json_array_unref (message);
    }

  return TRUE;
}

static void
perform_load (CockpitPcpMetrics *self,
              gint64 timestamp)
{
  struct timeval stamp;
  pmResult *result;
  int rc;

  if (timestamp == 0)
    {
      stamp.tv_sec = 0x7fffffff;
      if (pmSetMode (PM_MODE_BACK, &stamp, 0) != 0)
        g_return_if_reached ();
      rc = pmFetchArchive (&result);
      if (rc < 0)
        {
          g_message ("%s: couldn't read from archive: %s", self->name, pmErrStr (self->context));
          cockpit_channel_close (COCKPIT_CHANNEL (self), "internal-error");
          return;
        }
      memcpy (&stamp, &result->timestamp, sizeof (stamp));
      pmFreeResult (result);
    }
  else
    {
      stamp.tv_sec = (timestamp / 1000);
      stamp.tv_usec = (timestamp % 1000) * 1000;
    }

  if (pmSetMode (PM_MODE_INTERP | PM_XTB_SET(PM_TIME_MSEC), &stamp, self->interval) != 0)
    g_return_if_reached ();

  if (on_idle_batch (self))
    self->idler = g_idle_add (on_idle_batch, self);
}

static void
cockpit_pcp_metrics_prepare (CockpitChannel *channel)
{
  CockpitPcpMetrics *self = COCKPIT_PCP_METRICS (channel);
  const gchar *problem = "protocol-error";
  const gchar *source;
  JsonObject *options;
  const char *name;
  gint64 timestamp;
  int type;
  int rc;
  int i;

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
  else if (g_str_equal (source, "system")) // NB: this is for pure shared-library PMDAs; limited
    {
      type = PM_CONTEXT_LOCAL;
      name = NULL;
    }
  else if (g_str_equal (source, "pmcd"))
    {
      type = PM_CONTEXT_HOST;
      name = "local:"; // prefer AF_UNIX, falls back to localhost
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

  self->pmdescs = g_new0 (pmDesc, self->numpmid);
  for (i = 0; i < self->numpmid; i++)
    {
      rc = pmLookupDesc (self->pmidlist[i], self->pmdescs + i);
      if (rc < 0)
        {
          g_warning ("%s: failed to lookup metric: %s: %s", self->name, self->metrics[i], pmErrStr (self->context));
          goto out;
        }
    }

  /* "interval" option */
  if (!cockpit_json_get_int (options, "interval", 1000, &self->interval))
    {
      g_warning ("%s: invalid \"interval\" option", self->name);
      goto out;
    }
  else if (self->interval <= 0 || self->interval > G_MAXINT)
    {
      g_warning ("%s: invalid \"interval\" value: %" G_GINT64_FORMAT, self->name, self->interval);
      goto out;
    }

  /* "timestamp" option */
  if (!cockpit_json_get_int (options, "timestamp", 0, &timestamp))
    {
      g_warning ("%s: invalid \"timestamp\" option", self->name);
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
      g_warning ("%s: invalid \"instances\" option", self->name);
      goto out;
    }
  if (self->instances)
    self->filter = g_hash_table_new_full (filter_pair_hash, filter_pair_equal, g_free, NULL);

  /* "limit" option */
  if (!cockpit_json_get_int (options, "instances", G_MAXINT64, &self->limit))
    {
      g_warning ("%s: invalid \"instances\" option", self->name);
      goto out;
    }
  else if (self->limit <= 0)
    {
      g_warning ("%s: invalid \"limit\" option value: %" G_GINT64_FORMAT, self->name, self->limit);
      goto out;
    }

  problem = NULL;
  if (type == PM_CONTEXT_ARCHIVE)
    perform_load (self, timestamp);

  else
    cockpit_metrics_metronome (COCKPIT_METRICS (self), timestamp, self->interval);
  cockpit_channel_ready (channel);

out:
  if (problem)
    cockpit_channel_close (channel, problem);
}

static void
cockpit_pcp_metrics_close (CockpitChannel *channel,
                           const gchar *problem)
{
  CockpitPcpMetrics *self = COCKPIT_PCP_METRICS (channel);

  if (self->idler)
    {
      g_source_remove (self->idler);
      self->idler = 0;
    }

  COCKPIT_CHANNEL_CLASS (cockpit_pcp_metrics_parent_class)->close (channel, problem);
}

static void
cockpit_pcp_metrics_dispose (GObject *object)
{
  CockpitPcpMetrics *self = COCKPIT_PCP_METRICS (object);

  if (self->idler)
    {
      g_source_remove (self->idler);
      self->idler = 0;
    }

  if (self->context >= 0)
    {
      pmDestroyContext (self->context);
      self->context = -1;
    }

  G_OBJECT_CLASS (cockpit_pcp_metrics_parent_class)->dispose (object);
}

static void
cockpit_pcp_metrics_finalize (GObject *object)
{
  CockpitPcpMetrics *self = COCKPIT_PCP_METRICS (object);

  if (self->idler)
    g_source_remove (self->idler);
  g_strfreev (self->metrics);
  g_strfreev (self->instances);
  g_free (self->pmidlist);

  G_OBJECT_CLASS (cockpit_pcp_metrics_parent_class)->finalize (object);
}

static void
cockpit_pcp_metrics_class_init (CockpitPcpMetricsClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  CockpitMetricsClass *metrics_class = COCKPIT_METRICS_CLASS (klass);
  CockpitChannelClass *channel_class = COCKPIT_CHANNEL_CLASS (klass);

  gobject_class->dispose = cockpit_pcp_metrics_dispose;
  gobject_class->finalize = cockpit_pcp_metrics_finalize;

  channel_class->prepare = cockpit_pcp_metrics_prepare;
  channel_class->close = cockpit_pcp_metrics_close;
  metrics_class->tick = cockpit_pcp_metrics_tick;
}
