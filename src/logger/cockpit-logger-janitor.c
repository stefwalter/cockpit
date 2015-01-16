/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2015 Red Hat, Inc.
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

/* This program rotates PCP archives.  This could be done with a small
   shell script but there is no good way to dump the time range of an
   archive in a sane format, and the job is simple enough to just do
   it all in C.
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <glob.h>

#include <pcp/pmapi.h>

int verbose;
double max_age_in_hours;

static void
usage ()
{
  fprintf (stderr, "usage: cockpit-logger-janitor [-v] DIR MAXAGE_IN_HOURS\n");
  exit (1);
}

static void
remove_archive (const char *path)
{
  char *pattern;
  glob_t matches;

  printf ("Removing archive %s\n", path);

  asprintf (&pattern, "%s.*", path);
  if (glob(pattern, GLOB_NOSORT, NULL, &matches) == 0)
    {
      for (int i = 0; i < matches.gl_pathc; i++)
        {
          if (verbose)
            printf ("Removing %s\n", matches.gl_pathv[i]);
          if (unlink (matches.gl_pathv[i]) < 0)
            fprintf (stderr, "Can't remove %s: %m\n", matches.gl_pathv[i]);
        }
    }
  globfree (&matches);
  free (pattern);
}

static void
handle_archive (const char *path)
{
  int context, rc;
  pmLogLabel label;
  struct timeval end;
  time_t now = time (NULL);

  context = pmNewContext (PM_CONTEXT_ARCHIVE, path);
  if (context < 0)
    {
      fprintf (stderr, "%s: %s", path, pmErrStr (context));
      return;
    }

  rc = pmGetArchiveLabel (&label);
  if (rc < 0)
    {
      fprintf (stderr, "%s: %s", path, pmErrStr (rc));
      return;
    }

  rc = pmGetArchiveEnd (&end);
  if (rc < 0)
    {
      fprintf (stderr, "%s: %s", path, pmErrStr (rc));
      return;
    }

  pmDestroyContext (context);

  if (verbose)
    {
      printf ("%s: %ld - %ld (%g hours long, until %g hours ago)\n",
              path,
              label.ll_start.tv_sec, end.tv_sec,
              (end.tv_sec - label.ll_start.tv_sec) / 3600.0,
              (now - end.tv_sec) / 3600.0);
    }

  if ((now - end.tv_sec) / 3600.0 > max_age_in_hours)
    remove_archive (path);
}

static int
has_suffix (const char *name, const char *suffix)
{
  int nl = strlen (name);
  int sl = strlen (suffix);
  return nl > sl && strcmp (name + nl - sl, suffix) == 0;
}

int
main (int argc, char **argv)
{
  DIR *d;
  struct dirent *ent;

  if (argc > 1 && strcmp (argv[1], "-v") == 0)
    {
      verbose = 1;
      argc--;
      argv++;
    }

  if (argc != 3)
    usage ();

  max_age_in_hours = atof (argv[2]);
  if (max_age_in_hours == 0)
    usage ();

  d = opendir (argv[1]);
  while ((ent = readdir (d)))
    {
      if (has_suffix (ent->d_name, ".meta"))
        {
          char *path;
          asprintf (&path, "%s/%s", argv[1], ent->d_name);
          path[strlen(path) - strlen(".meta")] = '\0';
          handle_archive (path);
          free (path);
        }
    }
  closedir (d);
  return 0;
}
