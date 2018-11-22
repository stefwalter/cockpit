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

#include <keyutils.h>

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
  size_t auth_prefix_length = strlen (auth_prefix);
  const char *auth_suffix = "\"}";
  size_t auth_suffix_length = strlen (auth_suffix);
  unsigned char *message;
  ssize_t len;

  debug ("reading authorize message");

  len = cockpit_frame_read (STDIN_FILENO, &message);
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
      warn ("couldn't describe cockpit/session-token secret key: %s");
      return NULL;
    }
  if (strncmp (buffer, "user;0;0;001f0000;", 18) != 0)
    {
      warn ("kernel cockpit/session-token secret key has invalid permissions: %s");
      free (buffer);
      return NULL;
    }

  free (buffer);

  /* null-terminates */
  if (keyctl_read_alloc (key, (void **)secret) < 0)
    {
      warn ("couldn't read kernel cockpit/session-token secret key: %s");
      return NULL;
    }

  return secret;
}

static int
perform_basic (const char *authorization)
{
  struct pam_conv conv = { pam_conv_func, };
  pam_handle_t *pamh;
  char *password = NULL;
  char *user = NULL;
  int res;

  debug ("basic authentication");

  /* The input should be a user:password */
  password = cockpit_authorize_parse_basic (authorization, &user);

  secret = read_keyring_token ();
  ret = secret && password && strcmp (secret, password) == 0;

  if (secret)
    cockpit_memory_clear (secret, strlen (secret));
  if (password)
    cockpit_memory_clear (password, strlen (password));

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
  char *argv[] = { "cockpit-bridge", NULL };
  char *authorization;
  char *type = NULL;
  char **env;
  int status;
  int res;
  int i;

  cockpit_authorize_logger (authorize_logger, DEBUG_SESSION);

  /* Request authorization header */
  printf ("61\n{\"command\":\"authorize\",\"cookie\":\"local\",\"challenge\":\"basic\"}");

  /* And get back the authorization header */
  authorization = read_authorize_response ("authorization");
  if (!cockpit_authorize_type (authorization, &type))
    errx (EX, "invalid authorization header received");

  if (strcmp (type, "basic") != 0)
    errx (2, "unrecognized authentication method: %s", type);

  if (!perform_basic (authorization))
    {
      printf ("65\n{\"command\":\"init\",\"version\":1,\"problem\":\"authentication-failed\"}");
      exit (5);
    }

  cockpit_memory_clear (authorization, -1);
  free (authorization);
  free (type);

  debug ("executing bridge: %s", argv[0]);

  execvp (argv[0], argv);

  /* Should normally not return */
  warn ("can't exec %s", argv[0]);
  printf ("58\n{\"command\":\"init\",\"version\":1,\"problem\":\"internal-error\"}");
  exit (5);
}
