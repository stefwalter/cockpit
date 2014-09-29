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

#include "cockpitmodule.h"

#include "common/cockpittest.h"

#include <string.h>

extern const gchar **cockpit_agent_data_dirs;

typedef struct {
  GHashTable *listing;
  JsonObject *json;
} TestCase;

typedef struct {
  const gchar *datadirs[8];
  gboolean no_listing;
} Fixture;

static void
setup (TestCase *tc,
       gconstpointer data)
{
  const Fixture *fixture = data;

  if (fixture && fixture->datadirs[0])
    cockpit_agent_data_dirs = (const gchar **)fixture->datadirs;

  if (!fixture || !fixture->no_listing)
    tc->listing = cockpit_module_listing (&tc->json);
}

static void
teardown (TestCase *tc,
          gconstpointer data)
{
  cockpit_assert_expected ();

  if (tc->listing)
    g_hash_table_unref (tc->listing);
  if (tc->json)
    json_object_unref (tc->json);

  cockpit_agent_data_dirs = NULL;
}

static const Fixture fixture_listing = {
  .no_listing = TRUE
};

static void
test_listing (TestCase *tc,
              gconstpointer fixture)
{
  GHashTable *listing;
  JsonObject *json;

  g_assert (fixture == &fixture_listing);

  listing = cockpit_module_listing (&json);

  cockpit_assert_json_eq (json,
                          "{"
                          " \"test\": {"
                          "    \"checksum\": \"4784b8b983691a87886ce8325bda5f0ed748f058\","
                          "    \"manifest\" : { \"description\" : \"dummy\"}"
                          " },"
                          " \"second\": {"
                          "    \"checksum\": \"c47ad566aec6bebbbe4b4d93213f60389de78101\","
                          "    \"manifest\": { \"description\": \"second dummy description\"}"
                          " },"
                          " \"another\": {\"manifest\" : { \"description\" : \"another\"} }"
                          "}");

  g_hash_table_unref (listing);
  json_object_unref (json);
}

static void
test_resolve (TestCase *tc,
              gconstpointer fixture)
{
  gchar *path;

  path = cockpit_module_resolve (tc->listing, "test", "/sub/file.ext");
  g_assert_cmpstr (SRCDIR "/src/agent/mock-resource/system/cockpit/test/sub/file.ext", ==, path);
  g_free (path);
}

static void
test_resolve_bad_dots (TestCase *tc,
                       gconstpointer fixture)
{
  gchar *path;

  cockpit_expect_message ("invalid 'path' used as a resource: *");

  path = cockpit_module_resolve (tc->listing, "test", "../test/sub/file.ext");
  g_assert (path == NULL);
}

static void
test_resolve_bad_path (TestCase *tc,
                       gconstpointer fixture)
{
  gchar *path;

  cockpit_expect_message ("invalid 'path' used as a resource: *");

  path = cockpit_module_resolve (tc->listing, "test", "/sub/#file.ext");
  g_assert (path == NULL);
}

static void
test_resolve_bad_module (TestCase *tc,
                         gconstpointer fixture)
{
  gchar *path;

  cockpit_expect_message ("invalid 'module' name: *");

  path = cockpit_module_resolve (tc->listing, "#test", "/sub/file.ext");
  g_assert (path == NULL);
}

static void
test_resolve_not_found (TestCase *tc,
                        gconstpointer fixture)
{
  gchar *path;

  path = cockpit_module_resolve (tc->listing, "unknown", "/sub/file.ext");
  g_assert (path == NULL);
}

static void
test_expand (TestCase *tc,
             gconstpointer fixture)
{
  const gchar *data = "Depend on @@test@@ here @@another@@ and @@invalid@@";
  GQueue queue = G_QUEUE_INIT;
  GBytes *bytes;

  bytes = g_bytes_new_static (data, strlen (data));
  cockpit_module_expand (tc->listing, NULL, bytes, &queue);
  g_bytes_unref (bytes);

  cockpit_assert_bytes_eq (queue.head->data, "Depend on ", -1);
  cockpit_assert_bytes_eq (queue.head->next->data, "4784b8b983691a87886ce8325bda5f0ed748f058", -1);
  cockpit_assert_bytes_eq (queue.head->next->next->data, " here ", -1);
  cockpit_assert_bytes_eq (queue.head->next->next->next->data, "another", -1);
  cockpit_assert_bytes_eq (queue.head->next->next->next->next->data, " and ", -1);
  cockpit_assert_bytes_eq (queue.head->next->next->next->next->next->data, "", -1);
  g_assert (queue.head->next->next->next->next->next->next == NULL);

  while (!g_queue_is_empty (&queue))
    g_bytes_unref (g_queue_pop_head (&queue));
}

static void
test_expand_with_host (TestCase *tc,
                       gconstpointer fixture)
{
  const gchar *data = "Depend on @@test@@ here @@another@@ and @@invalid@@";
  GQueue queue = G_QUEUE_INIT;
  GBytes *bytes;

  bytes = g_bytes_new_static (data, strlen (data));
  cockpit_module_expand (tc->listing, "host", bytes, &queue);
  g_bytes_unref (bytes);

  cockpit_assert_bytes_eq (queue.head->data, "Depend on ", -1);
  cockpit_assert_bytes_eq (queue.head->next->data, "4784b8b983691a87886ce8325bda5f0ed748f058", -1);
  cockpit_assert_bytes_eq (queue.head->next->next->data, " here ", -1);
  cockpit_assert_bytes_eq (queue.head->next->next->next->data, "another@host", -1);
  cockpit_assert_bytes_eq (queue.head->next->next->next->next->data, " and ", -1);
  cockpit_assert_bytes_eq (queue.head->next->next->next->next->next->data, "", -1);
  g_assert (queue.head->next->next->next->next->next->next == NULL);

  while (!g_queue_is_empty (&queue))
    g_bytes_unref (g_queue_pop_head (&queue));
}

static const Fixture fixture_list_bad_directory = {
    .datadirs = { SRCDIR "/src/agent/mock-resource/bad-directory", NULL },
    .no_listing = TRUE
};

static const Fixture fixture_list_bad_file = {
    .datadirs = { SRCDIR "/src/agent/mock-resource/bad-file", NULL },
    .no_listing = TRUE
};

static const Fixture fixture_list_bad_name = {
    .datadirs = { SRCDIR "/src/agent/mock-resource/bad-module", NULL },
    .no_listing = TRUE
};

static void
test_list_bad_name (TestCase *tc,
                    gconstpointer fixture)
{
  GHashTable *listing;
  JsonObject *json;

  cockpit_expect_warning ("module * invalid *name*");

  listing = cockpit_module_listing (&json);

  cockpit_assert_json_eq (json,
                          "{"
                          " \"ok\": {"
                          "    \"checksum\": \"1ec8205ba984ef24edf33a3861eaef2a23663d14\","
                          "    \"manifest\" : { }"
                          " }"
                          "}");

  g_hash_table_unref (listing);
  json_object_unref (json);
}

int
main (int argc,
      char *argv[])
{
  g_setenv ("XDG_DATA_DIRS", SRCDIR "/src/agent/mock-resource/system", TRUE);
  g_setenv ("XDG_DATA_HOME", SRCDIR "/src/agent/mock-resource/home", TRUE);

  cockpit_test_init (&argc, &argv);

  g_test_add ("/module/listing", TestCase, &fixture_listing,
              setup, test_listing, teardown);
  g_test_add ("/resource/listing/bad-directory", TestCase, &fixture_list_bad_directory,
              setup, test_list_bad_name, teardown);
  g_test_add ("/resource/listing/bad-file", TestCase, &fixture_list_bad_file,
              setup, test_list_bad_name, teardown);
  g_test_add ("/resource/listing/bad-name", TestCase, &fixture_list_bad_name,
              setup, test_list_bad_name, teardown);

  g_test_add ("/module/resolve/simple", TestCase, NULL,
              setup, test_resolve, teardown);
  g_test_add ("/module/resolve/bad-dots", TestCase, NULL,
              setup, test_resolve_bad_dots, teardown);
  g_test_add ("/module/resolve/bad-path", TestCase, NULL,
              setup, test_resolve_bad_path, teardown);
  g_test_add ("/module/resolve/bad-module", TestCase, NULL,
              setup, test_resolve_bad_module, teardown);
  g_test_add ("/module/resolve/not-found", TestCase, NULL,
              setup, test_resolve_not_found, teardown);

  g_test_add ("/module/expand/simple", TestCase, NULL,
              setup, test_expand, teardown);
  g_test_add ("/module/expand/with-host", TestCase, NULL,
              setup, test_expand_with_host, teardown);

  return g_test_run ();
}
