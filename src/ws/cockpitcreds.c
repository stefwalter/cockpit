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
};

G_DEFINE_BOXED_TYPE (CockpitCreds, cockpit_creds, cockpit_creds_ref, cockpit_creds_unref);

static CockpitCreds *
cockpit_creds_new (void)
{
  CockpitCreds *creds = g_new0 (CockpitCreds, 1);
  creds->refs = 1;
  return creds;
}

static void
cockpit_creds_free (gpointer data)
{
  CockpitCreds *creds = data;
  g_free (creds->user);
  g_free (creds->password);
  g_free (creds);
}

CockpitCreds *
cockpit_creds_take_password (gchar *user,
                             gchar *password)
{
  CockpitCreds *creds = cockpit_creds_new ();
  creds->user = user;
  creds->password = password;
  return creds;
}

CockpitCreds *
cockpit_creds_new_password (const gchar *user,
                            const gchar *password)
{
  return cockpit_creds_take_password (g_strdup (user), g_strdup (password));
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

CockpitCreds *
cockpit_creds_new_gssapi (gss_name_t name,
                          gss_cred_id_t client)
{
  OM_uint32 major, minor;
  CockpitCreds *creds;
  krb5_context krb = NULL;
  krb5_error_code code;
  gss_buffer_desc display = { 0, NULL };
  krb5_principal principal = NULL;
  gchar *user = NULL;

  major = gss_display_name (&minor, name, &display, NULL);
  g_return_val_if_fail (!GSS_ERROR (major), NULL);

  code = krb5_init_context (&krb);
  if (code != 0) {
      g_critical ("Couldn't initialize krb5 context: %s",
                  krb5_get_error_message (NULL, code));
      goto out;
  }

  code = krb5_parse_name (krb, display.value, &principal);
  if (code != 0) {
      g_warning ("Couldn't parse name as kerberos principal: %s: %s",
                 (gchar *)display.value, krb5_get_error_message (krb, code));
      goto out;
  }

  user = g_malloc0 (LOGIN_NAME_MAX + 1);
  code = krb5_aname_to_localname (krb, principal, LOGIN_NAME_MAX, user);
  if (code == KRB5_LNAME_NOTRANS)
    {
      g_info ("No local user mapping for kerberos principal '%s'",
              (gchar *)display.value);
      g_free (user);
      user = g_strdup (display.value);
    }
  else if (code != 0)
    {
      g_warning ("Couldn't map kerberos principal '%s' to user: %s",
                 (gchar *)display.value, krb5_get_error_message (krb, code));
      g_free (user);
      user = NULL;
    }
  else
    {
      g_debug ("Mapped kerberos principal '%s' to user '%s'",
               (gchar *)display.value, user);
    }

  if (user)
    {
      creds = cockpit_creds_new ();
      creds->user = user;
      user = NULL;
    }

out:
  if (principal)
    krb5_free_principal (krb, principal);
  if (krb)
    krb5_free_context (krb);
  if (display.value)
    gss_release_buffer (&minor, &display);
  if (user)
    g_free (user);

  return creds;
}
