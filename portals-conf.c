/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "portals-conf.h"

#include <errno.h>

#define GROUP_NAME    "preferred"
#define BACKGROUND_KEY "org.freedesktop.impl.portal.Background"
#define TOKEN          "phray"

static char *
portals_conf_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "xdg-desktop-portal", "phosh-portals.conf", NULL);
}


static char *
generic_portals_conf_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "xdg-desktop-portal", "portals.conf", NULL);
}


/*
 * True if `list` (a semicolon-separated portals.conf value, e.g.
 * "gnome;phray;") contains TOKEN as an exact token. A plain strstr() would also
 * match backends whose names contain TOKEN.
 */
static gboolean
list_contains_token (const char *list)
{
  g_auto (GStrv) tokens = NULL;

  if (!list || !*list)
    return FALSE;

  tokens = g_strsplit (list, ";", -1);
  for (guint i = 0; tokens[i]; i++) {
    g_strstrip (tokens[i]);
    if (g_strcmp0 (tokens[i], TOKEN) == 0)
      return TRUE;
  }
  return FALSE;
}


/*
 * Loads the keyfile at `path` (phosh-portals.conf). A missing file is
 * tolerated; other errors are logged and return NULL, leaving the file alone
 * rather than risking overwriting something not understood.
 *
 * If `path` is missing and the generic portals.conf exists, its content is
 * loaded as the starting point instead of an empty keyfile (see
 * portals-conf.h).
 */
static GKeyFile *
load_or_new (const char *path)
{
  GKeyFile *keyfile = g_key_file_new ();
  g_autoptr (GError) error = NULL;

  if (g_key_file_load_from_file (keyfile, path,
                                  G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                  &error))
    return keyfile;

  if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
    g_warning ("Failed to read %s, leaving it untouched: %s", path, error->message);
    g_key_file_free (keyfile);
    return NULL;
  }

  /*
   * `path` is missing: try the generic file as the starting point, else an
   * empty keyfile.
   */
  {
    g_autofree char *generic_path = generic_portals_conf_path ();
    g_autoptr (GError) generic_error = NULL;

    if (g_key_file_load_from_file (keyfile, generic_path,
                                    G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                    &generic_error))
      return keyfile;

    /*
     * Any failure (missing, unreadable, malformed) falls through to the empty
     * keyfile above. No warning is logged, since a missing generic file is the
     * common case.
     */
  }

  return keyfile;
}


static void
save (GKeyFile *keyfile, const char *path)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr (GError) error = NULL;

  if (g_mkdir_with_parents (dir, 0700) != 0) {
    g_warning ("Failed to create %s: %s", dir, g_strerror (errno));
    return;
  }

  if (!g_key_file_save_to_file (keyfile, path, &error))
    g_warning ("Failed to write %s: %s", path, error->message);
}


void
phosh_portals_conf_enable_background_backend (void)
{
  g_autofree char *path = portals_conf_path ();
  g_autoptr (GKeyFile) keyfile = NULL;
  g_autofree char *current = NULL;
  g_autofree char *new_value = NULL;

  keyfile = load_or_new (path);
  if (!keyfile)
    return;

  current = g_key_file_get_string (keyfile, GROUP_NAME, BACKGROUND_KEY, NULL);

  if (list_contains_token (current))
    return;

  if (!current || !*current) {
    /*
     * No existing preference: list phosh first so Phosh's backend takes over if
     * it gains Background support, with phray as the fallback.
     */
    new_value = g_strdup ("phosh;" TOKEN ";");
  } else if (g_str_has_suffix (current, ";")) {
    new_value = g_strconcat (current, TOKEN ";", NULL);
  } else {
    new_value = g_strconcat (current, ";" TOKEN ";", NULL);
  }

  g_key_file_set_string (keyfile, GROUP_NAME, BACKGROUND_KEY, new_value);
  save (keyfile, path);
}


void
phosh_portals_conf_disable_background_backend (void)
{
  g_autofree char *path = portals_conf_path ();
  g_autoptr (GKeyFile) keyfile = NULL;
  g_autofree char *current = NULL;
  g_auto (GStrv) tokens = NULL;
  GString *new_value = NULL;
  gboolean any_kept = FALSE;

  keyfile = load_or_new (path);
  if (!keyfile)
    return;

  current = g_key_file_get_string (keyfile, GROUP_NAME, BACKGROUND_KEY, NULL);
  if (!list_contains_token (current))
    return;

  tokens = g_strsplit (current, ";", -1);
  new_value = g_string_new (NULL);
  for (guint i = 0; tokens[i]; i++) {
    g_strstrip (tokens[i]);
    if (tokens[i][0] == '\0' || g_strcmp0 (tokens[i], TOKEN) == 0)
      continue;
    g_string_append (new_value, tokens[i]);
    g_string_append_c (new_value, ';');
    any_kept = TRUE;
  }

  if (any_kept) {
    g_key_file_set_string (keyfile, GROUP_NAME, BACKGROUND_KEY, new_value->str);
  } else {
    g_key_file_remove_key (keyfile, GROUP_NAME, BACKGROUND_KEY, NULL);
  }
  g_string_free (new_value, TRUE);

  save (keyfile, path);
}
