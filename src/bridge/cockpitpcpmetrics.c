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
#include "pcpunits.h"

#include <pcp/pmapi.h>

/**
 * CockpitPcpMetrics:
 *
 * A #CockpitMetrics channel that pulls data from PCP
 */

#define COCKPIT_PCP_METRICS(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), COCKPIT_TYPE_PCP_METRICS, CockpitPcpMetrics))

typedef struct {
  const gchar *name;
  pmID id;
  pmDesc desc;
  pmUnits *units;
  gdouble factor;

  pmUnits units_buf;
} MetricInfo;

typedef struct {
  CockpitMetrics parent;
  const gchar *name;
  int context;
  int numpmid;
  pmID *pmidlist;
  MetricInfo *metrics;
  gchar **instances;
  gint64 interval;
  gint64 limit;
  guint idler;

  /* The previous samples sent */
  pmResult *last;
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

static gboolean
result_meta_equal (pmResult *r1,
                   pmResult *r2)
{
  pmValueSet *vs1;
  pmValueSet *vs2;
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
          if (vs1->vlist[j].inst != vs2->vlist[j].inst)
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
  JsonArray *metrics;
  JsonObject *metric;
  JsonArray *instances;
  JsonObject *root;
  pmValueSet *vs;
  gint64 timestamp;
  char *instance;
  int i, j;
  int rc;

  timestamp = (result->timestamp.tv_sec * 1000) +
              (result->timestamp.tv_usec / 1000);

  root = json_object_new ();
  json_object_set_int_member (root, "timestamp", timestamp);
  json_object_set_int_member (root, "interval", self->interval);

  metrics = json_array_new ();
  for (i = 0; i < result->numpmid; i++)
    {
      metric = json_object_new ();

      /* Name
       */
      json_object_set_string_member (metric, "name", self->metrics[i].name);

      /* Instances
       */
      vs = result->vset[i];
      if (vs->numval < 0 || (vs->numval == 1 && vs->vlist[0].inst == PM_IN_NULL))
        {
          /* When negative numval is an error code ... we don't care */
        }
      else
        {
          instances = json_array_new ();

          for (j = 0; j < vs->numval; j++)
            {
              /* PCP guarantees that the result is in the same order as requested */
              rc = pmNameInDom (self->metrics[i].desc.indom, vs->vlist[j].inst, &instance);
              if (rc != 0)
                {
                  g_warning ("%s: instance name lookup failed: %s", self->name, pmErrStr (rc));
                  instance = NULL;
                }

              /* HACK: We can't use json_builder_add_string_value here since
                 it turns empty strings into 'null' values inside arrays.

                 https://bugzilla.gnome.org/show_bug.cgi?id=730803
              */
              {
                JsonNode *string_element = json_node_alloc ();
                json_node_init_string (string_element, instance);
                json_array_add_element (instances, string_element);
              }

              if (instance)
                free (instance);
            }
          json_object_set_array_member (metric, "instances", instances);
        }

      /* Units
       */
      if (self->metrics[i].factor == 1.0)
        {
          json_object_set_string_member (metric, "units", pmUnitsStr(self->metrics[i].units));
        }
      else
        {
          gchar *name = g_strdup_printf ("%s*%g", pmUnitsStr(self->metrics[i].units), 1.0/self->metrics[i].factor);
          json_object_set_string_member (metric, "units", name);
          g_free (name);
        }

      /* Type
       */
      switch (self->metrics[i].desc.type) {
      case PM_TYPE_STRING:
        json_object_set_string_member (metric, "type", "string");
        break;
      case PM_TYPE_32:
      case PM_TYPE_U32:
      case PM_TYPE_64:
      case PM_TYPE_U64:
      case PM_TYPE_FLOAT:
      case PM_TYPE_DOUBLE:
        json_object_set_string_member (metric, "type", "number");
        break;
      default:
        break;
      }

      /* Semantics
       */
      switch (self->metrics[i].desc.sem) {
      case PM_SEM_COUNTER:
        json_object_set_string_member (metric, "semantics", "counter");
        break;
      case PM_SEM_INSTANT:
        json_object_set_string_member (metric, "semantics", "instant");
        break;
      case PM_SEM_DISCRETE:
        json_object_set_string_member (metric, "semantics", "discrete");
        break;
      default:
        break;
      }

      json_array_add_object_element (metrics, metric);
    }

  json_object_set_array_member (root, "metrics", metrics);
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

typedef struct {
  JsonArray *array;
  int n_skip;
} CompressedArrayBuilder;

static void
compressed_array_builder_init (CompressedArrayBuilder *compr)
{
  compr->array = NULL;
  compr->n_skip = 0;
}

static void
compressed_array_builder_add (CompressedArrayBuilder *compr, JsonNode *element)
{
  if (element == NULL)
    compr->n_skip++;
  else
    {
      if (!compr->array)
        compr->array = json_array_new ();
      for (int i = 0; i < compr->n_skip; i++)
        json_array_add_null_element (compr->array);
      compr->n_skip = 0;
      json_array_add_element (compr->array, element);
    }
}

static void
compressed_array_builder_take_and_add_array (CompressedArrayBuilder *compr, JsonArray *array)
{
  JsonNode *node = json_node_alloc ();
  json_node_init_array (node, array);
  compressed_array_builder_add (compr, node);
  json_array_unref (array);
}

static JsonArray *
compressed_array_builder_finish (CompressedArrayBuilder *compr)
{
  if (compr->array)
    return compr->array;
  else
    return json_array_new ();
}

static gboolean
result_value_equal (int valfmt,
                    pmValue *val1,
                    pmValue *val2)
{
  if (valfmt == PM_VAL_INSITU)
    return val1->value.lval == val2->value.lval;
  else
    return (val1->value.pval->vlen == val2->value.pval->vlen
            && memcmp (val1->value.pval, val2->value.pval, val1->value.pval->vlen) == 0);
}

static JsonNode *
build_sample (CockpitPcpMetrics *self,
              pmResult *result,
              int metric,
              int instance)
{
  MetricInfo *info = &self->metrics[metric];
  int valfmt = result->vset[metric]->valfmt;
  pmValue *value = &result->vset[metric]->vlist[instance];
  pmAtomValue sample;

  /* The following mouth full will set sample.d to the appropriate
     value, or return early for special cases and errors.
  */

  if (info->desc.type == PM_TYPE_AGGREGATE || info->desc.type == PM_TYPE_EVENT)
    return NULL;

  if (info->desc.sem == PM_SEM_COUNTER && info->desc.type != PM_TYPE_STRING)
    {
      if (!self->last)
        return NULL;

      pmAtomValue old, new;
      pmValue *last_value = &self->last->vset[metric]->vlist[instance];

      if (info->desc.type == PM_TYPE_64)
        {
          if (pmExtractValue (valfmt, value, PM_TYPE_64, &new, PM_TYPE_64) < 0
              || pmExtractValue (valfmt, last_value, PM_TYPE_64, &old, PM_TYPE_64) < 0)
            return json_node_new (JSON_NODE_NULL);

          sample.d = new.ll - old.ll;
        }
      else if (info->desc.type == PM_TYPE_U64)
        {
          if (pmExtractValue (valfmt, value, PM_TYPE_U64, &new, PM_TYPE_U64) < 0
              || pmExtractValue (valfmt, last_value, PM_TYPE_U64, &old, PM_TYPE_U64) < 0)
            return json_node_new (JSON_NODE_NULL);

          sample.d = new.ull - old.ull;
        }
      else
        {
          if (pmExtractValue (valfmt, value, info->desc.type, &new, PM_TYPE_DOUBLE) < 0
              || pmExtractValue (valfmt, last_value, info->desc.type, &old, PM_TYPE_DOUBLE) < 0)
            return json_node_new (JSON_NODE_NULL);

          sample.d = new.d - old.d;
        }
    }
  else
    {
      if (self->last)
        {
          pmValue *last_value = &self->last->vset[metric]->vlist[instance];
          if (result_value_equal (valfmt, value, last_value))
            return NULL;
        }

      if (info->desc.type == PM_TYPE_STRING)
        {
          if (pmExtractValue (valfmt, value, PM_TYPE_STRING, &sample, PM_TYPE_STRING) < 0)
            return json_node_new (JSON_NODE_NULL);

          JsonNode *node = json_node_new (JSON_NODE_VALUE);
          json_node_set_string (node, sample.cp);
          free (sample.cp);
          return node;
        }
      else
        {
          if (pmExtractValue (valfmt, value, info->desc.type, &sample, PM_TYPE_DOUBLE) < 0)
            return json_node_new (JSON_NODE_NULL);
        }
    }

  if (info->units != &info->desc.units)
    {
      if (pmConvScale (PM_TYPE_DOUBLE, &sample, &info->desc.units, &sample, info->units) < 0)
        return json_node_new (JSON_NODE_NULL);
      sample.d *= info->factor;
    }

  JsonNode *node = json_node_new (JSON_NODE_VALUE);
  json_node_set_double (node, sample.d);
  return node;
}

static JsonArray *
build_samples (CockpitPcpMetrics *self,
               pmResult *result)
{
  CompressedArrayBuilder samples;
  CompressedArrayBuilder array;
  pmValueSet *vs;
  int i, j;

  compressed_array_builder_init (&samples);

  for (i = 0; i < result->numpmid; i++)
    {
      vs = result->vset[i];

      /* When negative numval is an error code ... we don't care */
      if (vs->numval < 0)
        compressed_array_builder_add (&samples, NULL);
      else if (vs->numval == 1 && vs->vlist[0].inst == PM_IN_NULL)
        compressed_array_builder_add (&samples, build_sample (self, result, i, 0));
      else
        {
          compressed_array_builder_init (&array);
          for (j = 0; j < vs->numval; j++)
            compressed_array_builder_add (&array, build_sample (self, result, i, j));
          compressed_array_builder_take_and_add_array (&samples, compressed_array_builder_finish (&array));
        }
    }

  return compressed_array_builder_finish (&samples);
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

      /* We can't compress across a meta message.
       */
      if (self->last)
        pmFreeResult (self->last);
      self->last = NULL;
    }

  /* Send one set of samples */
  message = json_array_new ();
  json_array_add_array_element (message, build_samples (self, result));
  send_array (self, message);
  json_array_unref (message);

  if (self->last)
    pmFreeResult (self->last);
  self->last = result;

  /* Sent enough samples? */
  self->limit--;
  if (self->limit <= 0)
    cockpit_channel_close (COCKPIT_CHANNEL (self), NULL);
}

static gboolean units_equal (pmUnits *a,
                             pmUnits *b)
{
  return (a->scaleCount == b->scaleCount &&
          a->scaleTime == b->scaleTime &&
          a->scaleSpace == b->scaleSpace &&
          a->dimCount == b->dimCount &&
          a->dimTime == b->dimTime &&
          a->dimSpace == b->dimSpace);
}

static gboolean units_convertible (pmUnits *a,
                                   pmUnits *b)
{
  pmAtomValue dummy;
  dummy.d = 0;
  return pmConvScale (PM_TYPE_DOUBLE, &dummy, a, &dummy, b) >= 0;
}

static void
cockpit_pcp_metrics_prepare (CockpitChannel *channel)
{
  CockpitPcpMetrics *self = COCKPIT_PCP_METRICS (channel);
  const gchar *problem = "protocol-error";
  JsonObject *options;
  const gchar *source;
  const gchar **instances = NULL;
  const gchar **omit_instances = NULL;
  JsonArray *metrics;
  const char *name;
  int type;
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
  else if (g_str_equal (source, "direct"))
    {
      type = PM_CONTEXT_LOCAL;
      name = NULL;
    }
  else if (g_str_equal (source, "pmcd"))
    {
      type = PM_CONTEXT_HOST;
      name = "local:";
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

  /* "instances" option */
  if (!cockpit_json_get_strv (options, "instances", NULL, (gchar ***)&instances))
    {
      g_warning ("%s: invalid \"instances\" option (not an array of strings)", self->name);
      goto out;
    }

  /* "omit-instances" option */
  if (!cockpit_json_get_strv (options, "omit-instances", NULL, (gchar ***)&omit_instances))
    {
      g_warning ("%s: invalid \"omit-instances\" option (not an array of strings)", self->name);
      goto out;
    }

  /* "metrics" option */
  self->numpmid = 0;
  if (!cockpit_json_get_array (options, "metrics", NULL, &metrics))
    {
      g_warning ("%s: invalid \"metrics\" option was specified (not an array)", self->name);
      goto out;
    }
  if (metrics)
    self->numpmid = json_array_get_length (metrics);

  self->pmidlist = g_new0 (pmID, self->numpmid);
  self->metrics = g_new0 (MetricInfo, self->numpmid);
  for (i = 0; i < self->numpmid; i++)
    {
      MetricInfo *info = &self->metrics[i];
      const gchar *units;
      const gchar *type;
      const gchar *semantics;

      JsonNode *node = json_array_get_element (metrics, i);
      if (json_node_get_value_type (node) == G_TYPE_STRING)
        {
          info->name = json_node_get_string (node);
          type = NULL;
          units = NULL;
          semantics = NULL;
        }
      else if (json_node_get_node_type (node) == JSON_NODE_OBJECT)
        {
          if (!cockpit_json_get_string (json_node_get_object (node), "name", NULL, &info->name)
              || info->name == NULL)
            {
              g_warning ("%s: invalid \"metrics\" option was specified (no name for metric %d)",
                         self->name, i);
              goto out;
            }

          if (!cockpit_json_get_string (json_node_get_object (node), "units", NULL, &units))
            {
              g_warning ("%s: invalid units for metric %s (not a string)",
                         self->name, info->name);
              goto out;
            }

          if (!cockpit_json_get_string (json_node_get_object (node), "type", NULL, &type))
            {
              g_warning ("%s: invalid type for metric %s (not a string)",
                         self->name, info->name);
              goto out;
            }

          if (!cockpit_json_get_string (json_node_get_object (node), "semantics", NULL, &semantics))
            {
              g_warning ("%s: invalid semantics for metric %s (not a string)",
                         self->name, info->name);
              goto out;
            }
        }
      else
        {
          g_warning ("%s: invalid \"metrics\" option was specified (neither string nor object for metric %d)",
                     self->name, i);
          goto out;
        }

      if (pmLookupName (1, (char **)&info->name, &info->id) < 0)
        {
          g_warning ("%s: no such metric: %s (%s)", self->name, info->name, pmErrStr (self->context));
          goto out;
        }

      if (pmLookupDesc (info->id, &info->desc) < 0)
        {
          g_warning ("%s: no such metric: %s (%s)", self->name, info->name, pmErrStr (self->context));
          goto out;
        }

      if (units)
        {
          if (type == NULL)
            type = "number";

          if (cockpit_pmParseUnitsStr (units, &info->units_buf, &info->factor) < 0)
            {
              g_warning ("%s: failed to parse units: %s", self->name, units);
              goto out;
            }

          if (!units_convertible (&info->desc.units, &info->units_buf))
            {
              g_warning ("%s: can't convert metric %s to units %s", self->name, info->name, units);
              goto out;
            }

          if (info->factor != 1.0 || !units_equal (&info->desc.units, &info->units_buf))
            info->units = &info->units_buf;
        }

      if (!info->units)
        {
          info->units = &info->desc.units;
          info->factor = 1.0;
        }

      if (g_strcmp0 (type, "number") == 0)
        {
          int dt = info->desc.type;
          if (!(dt == PM_TYPE_32 || dt == PM_TYPE_U32 ||
                dt == PM_TYPE_64 || dt == PM_TYPE_U64 ||
                dt == PM_TYPE_FLOAT || dt == PM_TYPE_DOUBLE))
            {
              g_warning ("%s: metric %s is not a number", self->name, info->name);
              goto out;
            }
        }
      else if (g_strcmp0 (type, "string") == 0)
        {
          if (info->desc.type != PM_TYPE_STRING)
            {
              g_warning ("%s: metric %s is not a string", self->name, info->name);
              goto out;
            }
        }
      else if (type != NULL)
        {
          g_warning ("%s: unsupported type %s", self->name, type);
          goto out;
        }

      if (g_strcmp0 (semantics, "counter") == 0)
        {
          if (info->desc.sem != PM_SEM_COUNTER)
            {
              g_warning ("%s: metric %s is not a counter", self->name, info->name);
              goto out;
            }
        }
      else if (g_strcmp0 (semantics, "instant") == 0)
        {
          if (info->desc.sem != PM_SEM_INSTANT)
            {
              g_warning ("%s: metric %s is not instantaneous", self->name, info->name);
              goto out;
            }
        }
      else if (g_strcmp0 (semantics, "discrete") == 0)
        {
          if (info->desc.sem != PM_SEM_DISCRETE)
            {
              g_warning ("%s: metric %s is not discrete", self->name, info->name);
              goto out;
            }
        }
      else if (semantics != NULL)
        {
          g_warning ("%s: unsupported semantics %s", self->name, semantics);
          goto out;
        }

      self->pmidlist[i] = info->id;

      if (info->desc.indom != PM_INDOM_NULL)
        {
          if (instances)
            {
              pmDelProfile (info->desc.indom, 0, NULL);
              for (int i = 0; instances[i]; i++)
                {
                  int instid = pmLookupInDom (info->desc.indom, instances[i]);
                  if (instid >= 0)
                    pmAddProfile (info->desc.indom, 1, &instid);
                }
            }
          else if (omit_instances)
            {
              pmAddProfile (info->desc.indom, 0, NULL);
              for (int i = 0; omit_instances[i]; i++)
                {
                  int instid = pmLookupInDom (info->desc.indom, omit_instances[i]);
                  if (instid >= 0)
                    pmDelProfile (info->desc.indom, 1, &instid);
                }
            }
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

  problem = NULL;
  cockpit_metrics_metronome (COCKPIT_METRICS (self), self->interval);
  cockpit_channel_ready (channel);

out:
  if (problem)
    cockpit_channel_close (channel, problem);
  g_free (instances);
  g_free (omit_instances);
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

  if (self->last)
    {
      pmFreeResult (self->last);
      self->last = NULL;
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

  g_free (self->metrics);
  g_free (self->instances);
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
