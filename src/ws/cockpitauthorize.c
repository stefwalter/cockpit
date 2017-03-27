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

#include "cockpitauthorize.h"
#include "cockpitbase64.h"

#include "common/cockpithex.h"
#include "common/cockpitmemory.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <crypt.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ----------------------------------------------------------------------------
 * Tools
 */

#ifndef debug
#define debug(format, ...) \
  do { if (logger_verbose) \
      message ("debug: " format, ##__VA_ARGS__); \
  } while (0)
#endif

static int logger_verbose = 0;
static void (* logger) (const char *data);

#ifndef message
#if __GNUC__ > 2
static void
message (const char *format, ...)
__attribute__((__format__(__printf__, 1, 2)));
#endif

static void
message (const char *format, ...)
{
  va_list va;
  char *data;
  int res;

  if (!logger)
    return;

  /* Fast path for simple messages */
  if (!strchr (format, '%'))
    {
      logger (format);
      return;
    }

  va_start (va, format);
  res = vasprintf (&data, format, va);
  va_end (va);

  if (res < 0)
    {
      logger ("out of memory printing message");
      return;
    }

  logger (data);
  free (data);
}
#endif

void
cockpit_authorize_logger (void (* func) (const char *data),
                          int verbose)
{
  logger_verbose = verbose;
  logger = func;
}

static void
secfree (void *data,
         ssize_t len)
{
  if (!data)
    return;

  cockpit_memory_clear (data, len);
  free (data);
}

static ssize_t
parse_salt (const char *input)
{
  const char *pos;
  const char *end;

  /*
   * Parse a encrypted secret produced by crypt() using one
   * of the additional algorithms. Return the length of
   * the salt or -1.
   */

  if (input[0] != '$')
    return -1;
  pos = strchr (input + 1, '$');
  if (pos == NULL || pos == input + 1)
    return -1;
  end = strchr (pos + 1, '$');
  if (end == NULL || end < pos + 8)
    return -1;

  /* Full length of the salt */
  return (end - input) + 1;
}

/* ----------------------------------------------------------------------------
 * Respond to challenges
 */

ssize_t
cockpit_authorize_type (const char *challenge,
                        char **type)
{
  size_t i, len;

  /*
   * Either a space or a colon is the delimiter
   * that splits the type from the remainder
   * of the content.
   */

  len = strcspn (challenge, ": ");
  if (len == 0 || challenge[len] == '\0')
    {
      message ("invalid \"authorize\" message");
      errno = EINVAL;
      return -1;
    }

  if (type)
    {
      *type = strndup (challenge, len);
      if (*type == NULL)
        {
          message ("couldn't allocate memory for \"authorize\" challenge");
          errno = ENOMEM;
          return -1;
        }
      for (i = 0; i < len; i++)
        (*type)[i] = tolower ((*type)[i]);
    }

  while (challenge[len] == ' ')
    len++;
  return len;
}

ssize_t
cockpit_authorize_subject (const char *input,
                           unsigned char **subject,
                           size_t *subject_len)
{
  size_t len;

  len = strcspn (input, ": ");
  if (len == 0 || input[len] == '\0')
    {
      message ("invalid \"authorize\" message \"challenge\": no subject");
      errno = EINVAL;
      return -1;
    }

  if (subject && subject_len)
    {
      *subject = cockpit_hex_decode (input, len, subject_len);
      if (!*subject)
        {
          message ("invalid \"authorize\" message \"challenge\": bad hex encoding");
          errno = EINVAL;
          return -1;
        }
    }

  while (input[len] == ' ')
    len++;
  return len;
}

char *
cockpit_authorize_crypt1 (const char *input,
                          const char *password)
{
  struct crypt_data *cd = NULL;
  char *response = NULL;
  char *nonce = NULL;
  char *salt = NULL;
  const char *npos;
  const char *spos;
  char *secret;
  char *resp;
  int errn = 0;

  npos = input;
  spos = strchr (npos, ':');

  if (spos == NULL)
    {
      message ("couldn't parse \"authorize\" message \"challenge\"");
      goto out;
    }

  nonce = strndup (npos, spos - npos);
  salt = strdup (spos + 1);
  if (!nonce || !salt)
    {
      message ("couldn't allocate memory for challenge fields");
      goto out;
    }

  if (parse_salt (nonce) < 0 ||
      parse_salt (salt) < 0)
    {
      message ("\"authorize\" message \"challenge\" has bad nonce or salt");
      errn = EINVAL;
      goto out;
    }

  cd = calloc (2, sizeof (struct crypt_data));
  if (cd == NULL)
    {
      message ("couldn't allocate crypt data");
      errn = ENOMEM;
      goto out;
    }

  /*
   * This is what we're generating here:
   *
   * response = "crypt1:" crypt(crypt(password, salt), nonce)
   */

  secret = crypt_r (password, salt, cd + 0);
  if (secret == NULL)
    {
      errn = errno;
      message ("couldn't hash password via crypt: %m");
      goto out;
    }

  resp = crypt_r (secret, nonce, cd + 1);
  if (resp == NULL)
    {
      errn = errno;
      message ("couldn't hash secret via crypt: %m");
      goto out;
    }

  if (asprintf (&response, "crypt1:%s", resp) < 0)
    {
      errn = ENOMEM;
      message ("couldn't allocate response");
      goto out;
    }

out:
  free (nonce);
  free (salt);
  secfree (cd, sizeof (struct crypt_data) * 2);

  if (!response)
    errno = errn;

  return response;
}

char *
cockpit_authorize_basic (const char *input,
                         const char **user)
{
  unsigned char *buf = NULL;
  size_t len;
  ssize_t res;
  int errn = 0;
  size_t off;

  len = strcspn (input, " ");
  buf = malloc (len + 1);
  if (!buf)
    {
      message ("couldn't allocate memory for Basic header");
      errn = ENOMEM;
      goto out;
    }

  /* Decode and find split point */
  res = cockpit_base64_pton (input, len, buf, len);
  if (res < 0)
    {
      message ("invalid base64 data in Basic header");
      errn = EINVAL;
      goto out;
    }
  assert (res <= len);
  buf[res] = 0;
  off = strcspn ((char *)buf, ":");
  if (off == 0 || off == res)
    {
      message ("invalid base64 data in Basic header");
      errn = EINVAL;
      goto out;
    }

  if (user)
    {
      *user = strndup ((char *)buf, off);
      if (!*user)
        {
          message ("couldn't allocate memory for user name");
          errn = ENOMEM;
          goto out;
        }
    }

  memmove (buf, buf + off + 1, res - (off + 1));

out:
  if (errn != 0)
    {
      errno = errn;
      free (buf);
      buf = NULL;
    }
  return (char *)buf;
}

void *
cockpit_authorize_negotiate (const char *input,
                             size_t *length)
{
  unsigned char *buf = NULL;
  size_t len;
  ssize_t res;

  len = strcspn (input, " ");
  buf = malloc (len + 1);
  if (!buf)
    {
      message ("couldn't allocate memory for Negotiate header");
      errno = ENOMEM;
      return NULL;
    }

  /* Decode and find split point */
  res = cockpit_base64_pton (input, len, buf, len);
  if (res < 0)
    {
      message ("invalid base64 data in Negotiate header");
      free (buf);
      errno = EINVAL;
      return NULL;
    }

  *length = res;
  return buf;
}
