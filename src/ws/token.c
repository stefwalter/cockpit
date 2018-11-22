/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2018 Red Hat, Inc.
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

#include "common/cockpitauthorize.h"
#include "common/cockpitframe.h"
#include "common/cockpitmemory.h"

#include <err.h>
#include <errno.h>
#include <keyutils.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* This program lets the user into the local already running session by checking
 * the the browser can access a shared secret.
 */

#define DEBUG_SESSION 0
#define EX 127

#if DEBUG_SESSION
#define debug(fmt, ...) (fprintf (stderr, "cockpit-session: " fmt "\n", ##__VA_ARGS__))
#else
#define debug(...)
#endif

#if     __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ > 4)
#define GNUC_NORETURN __attribute__((__noreturn__))
#else
#define GNUC_NORETURN
#endif

static char *
read_authorize_response (void)
{
  const char *auth_prefix = "\n{\"command\":\"authorize\",\"cookie\":\"local\",\"response\":\"";
  size_t auth_prefix_size = strlen (auth_prefix);
  const char *auth_suffix = "\"}";
  size_t auth_suffix_size = strlen (auth_suffix);
  unsigned char *message;
  ssize_t len;

  debug ("reading authorize message");

  len = cockpit_frame_read (0, &message);
  if (len < 0)
    err (127, "couldn't read \"authorize\" message");

  /* The authorize messages we receive always have an exact prefix and suffix */
  if (len <= auth_prefix_size + auth_suffix_size ||
      memcmp (message, auth_prefix, auth_prefix_size) != 0 ||
      memcmp (message + (len - auth_suffix_size), auth_suffix, auth_suffix_size) != 0)
    {
      errx (2, "didn't receive expected \"authorize\" message");
    }

  len -= auth_prefix_size + auth_suffix_size;
  memmove (message, message + auth_prefix_size, len);
  message[len] = '\0';
  return (char *)message;
}

static char *
read_keyring_token (void)
{
  key_serial_t key;
  char *buffer = NULL;
  char *secret = NULL;

  key = keyctl_search (KEY_SPEC_SESSION_KEYRING, "user", "cockpit/session-token", 0);
  if (key < 0)
    {
      /* missing key or revoked key is not an error */
      if (errno != ENOKEY && errno != EKEYREVOKED)
        warn ("failed to lookup cockpit/session-token secret key");
      return NULL;
    }

  if (keyctl_describe_alloc (key, &buffer) < 0)
    {
      warn ("couldn't describe cockpit/session-token secret key");
      return NULL;
    }
  if (strncmp (buffer, "user;0;0;001f0000;", 18) != 0)
    {
      warnx ("kernel cockpit/session-token secret key has invalid permissions");
      free (buffer);
      return NULL;
    }

  free (buffer);

  /* null-terminates */
  if (keyctl_read_alloc (key, (void **)secret) < 0)
    {
      warn ("couldn't read kernel cockpit/session-token secret key");
      return NULL;
    }

  return secret;
}

static int
perform_auth (const char *authorization)
{
  const char *challenge = NULL;
  const char *token = NULL;
  char *password = NULL;
  char *secret = NULL;
  char *user = NULL;
  char *type = NULL;
  int ret = 0;

  challenge = cockpit_authorize_type (authorization, &type);
  if (challenge)
    {
      if (strcmp (type, "basic") == 0)
        token = password = cockpit_authorize_parse_basic (authorization, &user);
      else if (strcmp (type, "token") == 0)
        token = challenge;
      else
        warnx ("unrecognized authentication method: %s", type);
    }

  secret = read_keyring_token ();
  ret = secret && token && strcmp (secret, token) == 0;

  if (secret)
    cockpit_memory_clear (secret, -1);
  free (secret);

  if (password)
    cockpit_memory_clear (password, -1);
  free (password);

  free (type);
  return ret;
}

static void
authorize_logger (const char *data)
{
  warnx ("%s", data);
}

int
main (int argc,
      char **argv)
{
  char *command[] = { "cockpit-bridge", NULL };
  char *authorization;

  cockpit_authorize_logger (authorize_logger, DEBUG_SESSION);

  /* Request authorization header */
  printf ("57\n{\"command\":\"authorize\",\"cookie\":\"local\",\"challenge\":\"*\"}");

  /* And get back the authorization header */
  authorization = read_authorize_response ();
  if (!perform_auth (authorization))
    {
      printf ("65\n{\"command\":\"init\",\"version\":1,\"problem\":\"authentication-failed\"}");
      exit (5);
    }

  cockpit_memory_clear (authorization, -1);
  free (authorization);

  debug ("executing bridge: %s", command[0]);

  execvp (command[0], command);

  /* Should normally not return */
  warn ("can't exec %s", argv[0]);
  printf ("58\n{\"command\":\"init\",\"version\":1,\"problem\":\"internal-error\"}");
  exit (5);
}
