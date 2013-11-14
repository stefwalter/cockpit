/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
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

#include "cockpitcreds.h"
#include "cockpit/cockpit.h"

#include <krb5/krb5.h>

struct _CockpitCreds {
  gint refs;
  gchar *user;
  gchar *password;
  gchar *rhost;
  gss_cred_id_t gssapi;
};

G_DEFINE_BOXED_TYPE (CockpitCreds, cockpit_creds, cockpit_creds_ref, cockpit_creds_unref);

static void
cockpit_creds_free (gpointer data)
{
  CockpitCreds *creds = data;
  OM_uint32 minor;

  g_free (creds->user);
  g_free (creds->password);
  g_free (creds->rhost);
  if (creds->gssapi != GSS_C_NO_CREDENTIAL)
    gss_release_cred (&minor, &creds->gssapi);
  g_free (creds);
}

/**
 * cockpit_creds_new:
 * @user: the user name the creds are for
 * @...: multiple credentials, followed by NULL
 *
 * Create a new set of credentials for a user. Each vararg should be
 * a COCKPIT_CRED_PASSWORD, COCKPIT_CRED_RHOST, or similar constant
 * followed by the value.
 *
 * Returns: (transfer full): the new set of credentials.
 */
CockpitCreds *
cockpit_creds_new (const gchar *user,
                   ...)
{
  CockpitCreds *creds;
  const char *type;
  va_list va;

  creds = g_new0 (CockpitCreds, 1);
  creds->user = g_strdup (user);

  va_start (va, user);
  for (;;)
    {
      type = va_arg (va, const char *);
      if (type == NULL)
        break;
      else if (g_str_equal (type, COCKPIT_CRED_PASSWORD))
        creds->password = g_strdup (va_arg (va, const char *));
      else if (g_str_equal (type, COCKPIT_CRED_RHOST))
        creds->rhost = g_strdup (va_arg (va, const char *));
      else if (g_str_equal (type, COCKPIT_CRED_GSSAPI))
        creds->gssapi = va_arg (va, gss_cred_id_t);
      else
        g_assert_not_reached ();
    }
  va_end (va);

  creds->refs = 1;
  return creds;
}

CockpitCreds *
cockpit_creds_ref (CockpitCreds *creds)
{
  g_return_val_if_fail (creds != NULL, NULL);
  g_atomic_int_inc (&creds->refs);
  return creds;
}

void
cockpit_creds_unref (gpointer creds)
{
  CockpitCreds *c = creds;
  g_return_if_fail (creds != NULL);
  if (g_atomic_int_dec_and_test (&c->refs))
    cockpit_creds_free (c);
}

const gchar *
cockpit_creds_get_user (CockpitCreds *creds)
{
  g_return_val_if_fail (creds != NULL, NULL);
  return creds->user;
}

const gchar *
cockpit_creds_get_password (CockpitCreds *creds)
{
  g_return_val_if_fail (creds != NULL, NULL);
  return creds->password;
}

/**
 * cockpit_creds_get_rhost:
 * @creds: the credentials
 *
 * Get the remote host credential, or NULL
 * if none present.
 *
 * Returns: the remote host or NULL
 */
const gchar *
cockpit_creds_get_rhost (CockpitCreds *creds)
{
  g_return_val_if_fail (creds != NULL, NULL);
  return creds->rhost;
}

/**
 * cockpit_creds_get_gssapi:
 * @creds: the credentials
 *
 * Get GSSAPI client credentials, or NULL if not present.
 *
 * Returns: the GSSAPI creds or NULL
 */
gss_cred_id_t
cockpit_creds_get_gssapi (CockpitCreds *creds)
{
  g_return_val_if_fail (creds != NULL, NULL);
  return creds->gssapi;
}

gboolean
cockpit_creds_equal (gconstpointer v1,
                     gconstpointer v2)
{
  const CockpitCreds *c1;
  const CockpitCreds *c2;

  if (v1 == v2)
    return TRUE;
  if (!v1 || !v2)
    return FALSE;

  c1 = v1;
  c2 = v2;

  return g_strcmp0 (c1->user, c2->user) == 0 &&
         g_strcmp0 (c1->password, c2->password) == 0 &&
         g_strcmp0 (c1->rhost, c2->rhost) == 0;
}

guint
cockpit_creds_hash (gconstpointer v)
{
  const CockpitCreds *c = v;
  guint hash = 0;
  if (v)
    {
      c = v;
      if (c->user)
        hash ^= g_str_hash (c->user);
      if (c->rhost)
        hash ^= g_str_hash (c->rhost);
    }
  return hash;
}

