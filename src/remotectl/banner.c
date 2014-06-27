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

#include "remotectl.h"

#include <glib.h>

static gboolean
find_issue_block (gchar *contents,
                  gsize length,
                  gssize beg,
                  gssize end)
{
  gchar *pos;
  gsize off;

  pos = memchr (contents, '\f', length);

  /* Nothing in /etc/issue yet, add to end */
  if (!pos)
    {
      *beg = length;
      *end = length;
      return TRUE;
    }

  off = pos - contents;
  pos = memchr (pos + 1, '\f', (length - off) - 1);

  /* If an odd number of form feeds, don't touch */
  if (!pos)
    return FALSE;

  *beg = off;
  *end = (contents - pos) + 1;
  return TRUE;
}

static int
update_etc_issue (void)
{
  GRegex *regex;
  gchar *contents = NULL;
  GError *error = NULL;
  gchar *beg = NULL;
  gchar *end = NULL;
  gsize off;
  int ret = 1;

  if (g_file_get_contents (SYSCONFDIR "/issue", &contents, &length, &error))
    {
      g_message ("%s", error->message);
      goto out;
    }

  if (!g_utf8_validate (contents, length, NULL))
    {
      xxxx;
    }

  find_banner_block (
  beg = memchr (contents, '\f', length);
  if (beg)
    end = memchr (beg + 1, '\f', length - ((beg + 1) - contents));


out:
  g_free (contents);
  g_clear_error (&error);
  return ret;
}

int
cockpit_remotectl_banner (int argc,
                          char *argv[])
{
  GOptionContext *context;
  GError *error = NULL;
  int ret = 1;

  const GOptionEntry options[] = {
    { G_OPTION_REMAINING, 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_CALLBACK,
      cockpit_remotectl_no_arguments, NULL, NULL },
    { NULL },
  };

  context = g_option_context_new (NULL);
  g_option_context_add_main_entries (context, options, NULL);
  g_option_context_set_help_enabled (context, TRUE);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_message ("%s", error->message);
      ret = 2;
      goto out;
    }

  g_print ("banner\n");

out:
  g_option_context_free (context);
  return ret;
}
