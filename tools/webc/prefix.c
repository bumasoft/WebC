// SPDX-License-Identifier: Apache-2.0
#include "prefix.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef WEBC_PREFIX
#define WEBC_PREFIX "."
#endif

static char g_prefix[PATH_MAX];
static bool g_resolved;

// A framework root is a directory with include/webc in it. That is the whole test:
// everything the CLI reads from the prefix (headers, libwebc.a, assets, skills,
// tools/client) lives in one tree, and the compiler cannot find one without the rest.
static bool has_framework(const char *dir) {
  char inc[PATH_MAX];
  snprintf(inc, sizeof inc, "%s/include/webc", dir);
  struct stat st;
  return stat(inc, &st) == 0 && S_ISDIR(st.st_mode);
}

// dir-name of a path, in place: "/a/b/webc" -> "/a/b", "webc" -> "."
static void dirname_of(char *path) {
  char *slash = strrchr(path, '/');
  if (!slash) {
    snprintf(path, PATH_MAX, ".");
    return;
  }
  if (slash == path) slash[1] = 0; // "/webc" -> "/"
  else *slash = 0;
}

// argv[0] with no slash is a name on the PATH; the binary it names is the one running
static bool from_path(const char *name, char *out, size_t n) {
  const char *path_env = getenv("PATH");
  if (!path_env || !strchr(path_env, ':')) return false;
  char dirs[4096];
  snprintf(dirs, sizeof dirs, "%s", path_env);
  for (char *tok = strtok(dirs, ":"); tok; tok = strtok(nullptr, ":")) {
    snprintf(out, n, "%s/%s", tok, name);
    struct stat st;
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return true;
  }
  return false;
}

void webc_prefix_init(const char *argv0) {
  if (g_resolved) return;
  g_resolved = true;

  // 1. the env that says where: a checkout, or a tree somebody moved on purpose
  const char *env = getenv("WEBC_HOME");
  if (env && *env && has_framework(env)) {
    snprintf(g_prefix, sizeof g_prefix, "%s", env);
    return;
  }

  // 2. the tree beside the binary, through symlinks: an install is bin/webc with
  //    include/ and build/ at the root, and ~/.local/bin/webc is a symlink into it
  if (argv0 && *argv0) {
    char self[PATH_MAX];
    if (!strchr(argv0, '/')) {
      if (!from_path(argv0, self, sizeof self)) self[0] = 0;
    } else {
      snprintf(self, sizeof self, "%s", argv0);
    }
    char real[PATH_MAX];
    if (self[0] && realpath(self, real)) {
      char dir[PATH_MAX];
      snprintf(dir, sizeof dir, "%s", real);
      dirname_of(dir);
      static const char *cands[2];
      cands[0] = dir;
      char up[PATH_MAX];
      snprintf(up, sizeof up, "%s", dir);
      dirname_of(up);
      cands[1] = up;
      for (int i = 0; i < 2; i++) {
        if (has_framework(cands[i])) {
          snprintf(g_prefix, sizeof g_prefix, "%s", cands[i]);
          return;
        }
      }
    }
  }

  // 3. the path the build baked (the checkout the CLI was compiled in)
  snprintf(g_prefix, sizeof g_prefix, "%s", WEBC_PREFIX);
}

const char *webc_prefix(void) {
  if (!g_resolved) webc_prefix_init(nullptr);
  return g_prefix;
}
