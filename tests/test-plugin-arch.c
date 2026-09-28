// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for hplip_stage_plugin_libs() in hplip-plugin-arch.c.
//
// The architecture matching lives in the Printer Application's plugin install
// path, where the vendor archive's "<component>-<ARCH>.so" libraries are
// symlinked to "<component>.so" for the build architecture. This test builds
// the real module against a temp directory of fake vendor libraries and
// checks that the right libraries are staged for each release architecture
// (x86_64 for the amd64 release, arm64 for the arm64 release) and that
// mismatched-arch libraries are rejected - the behaviour issue #14 asks to
// verify. It needs only libc, so it builds without PAPPL or libcurl.
//
// Usage: compiled into a single binary by tests/run-plugin-arch-tests.sh.

#include "hplip-plugin-arch.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int checks = 0;
static int failures = 0;

static void
check(const char *what, int condition)
{
  checks ++;
  if (condition)
    printf("ok: %s\n", what);
  else
  {
    failures ++;
    printf("FAIL: %s\n", what);
  }
}

// Create a file with some bytes in it, so the "vendor library" is a real file
// that a symlink can point at.
static void
make_vendor_file(const char *dir, const char *name)
{
  char path[1024];
  FILE *f;

  snprintf(path, sizeof(path), "%s/%s", dir, name);
  f = fopen(path, "wb");
  if (f == NULL)
  {
    fprintf(stderr, "cannot create %s\n", path);
    exit(2);
  }
  fputs("vendor plugin object\n", f);
  fclose(f);
}

// Verify each expected "<component>.so" symlink exists and points back at its
// vendor file, then count every symlink actually present in plugin_tmp to be
// sure no unexpected one (e.g. a mismatched-arch library) was staged.
static void
check_staged(const char *plugin_tmp, int expected, const char **want,
             int want_count)
{
  DIR *d;
  struct dirent *entry;
  int staged = 0;
  int i;

  for (i = 0; i < want_count; i ++)
  {
    char link[1024];
    char target[1024];
    ssize_t n;

    snprintf(link, sizeof(link), "%s/%s.so", plugin_tmp, want[i]);
    n = readlink(link, target, sizeof(target) - 1);
    if (n < 0)
    {
      check(link " exists as a symlink", 0);
      continue;
    }
    target[n] = '\0';
    check(link " points back to its vendor file", strcmp(target, want[i]) == 0);
    staged ++;
  }

  d = opendir(plugin_tmp);
  if (d)
  {
    while ((entry = readdir(d)) != NULL)
    {
      char link[1024];
      struct stat st;

      if (entry->d_name[0] == '.')
        continue;
      snprintf(link, sizeof(link), "%s/%s", plugin_tmp, entry->d_name);
      if (lstat(link, &st) == 0 && S_ISLNK(st.st_mode))
        staged ++;
    }
    closedir(d);
  }

  check("symlinks present matches the return value", staged == expected);
}

// amd64 release: the vendor archive tags libraries with the "x86_64" label.
static void
test_x86_64(void)
{
  char plugin_dir[256];
  char plugin_tmp[1024];
  char target[1024];
  struct stat st;
  int rc;
  ssize_t n;
  const char *want[] = { "hpcups", "hp-scan" };

  snprintf(plugin_dir, sizeof(plugin_dir), "/tmp/hplip-arch-x86_64-XXXXXX");
  if (mkdtemp(plugin_dir) == NULL) { perror("mkdtemp"); exit(2); }
  snprintf(plugin_tmp, sizeof(plugin_tmp), "%s/plugin_tmp", plugin_dir);
  if (mkdir(plugin_tmp, 0755) != 0) { perror("mkdir"); exit(2); }

  make_vendor_file(plugin_tmp, "hpcups-x86_64.so");
  make_vendor_file(plugin_tmp, "hp-scan-x86_64.so");
  make_vendor_file(plugin_tmp, "hp-versioned-x86_64.so.3.22.10");
  make_vendor_file(plugin_tmp, "hpcups-arm64.so");
  make_vendor_file(plugin_tmp, "hp-scan-arm32.so");
  make_vendor_file(plugin_tmp, "hp-x86_32.so");
  make_vendor_file(plugin_tmp, "hp-only-arm64.so");
  make_vendor_file(plugin_tmp, "hpcups-x86_64.txt");

  rc = hplip_stage_plugin_libs(plugin_dir, "x86_64");
  check("x86_64 release stages exactly its three libraries", rc == 3);
  check_staged(plugin_tmp, 3, want, 2);

  {
    char link[1024];
    snprintf(link, sizeof(link), "%s/hp-versioned.so", plugin_tmp);
    n = readlink(link, target, sizeof(target) - 1);
    check("versioned .so.3.22.10 staged as hp-versioned.so",
	n > 0 && strcmp(target, "hp-versioned.so") == 0);
  }

  // A component which exists only as an arm64 library must not be staged
  // for the x86_64 release.
  {
    char link[1024];
    snprintf(link, sizeof(link), "%s/hp-only.so", plugin_tmp);
    if (lstat(link, &st) == 0)
      check("arm64-only library is not staged for x86_64", 0);
    else
      check("arm64-only library is not staged for x86_64", 1);
  }
}

// arm64 release: the vendor archive tags libraries with the "arm64" label.
static void
test_arm64(void)
{
  char plugin_dir[256];
  char plugin_tmp[1024];
  char target[1024];
  struct stat st;
  int rc;
  ssize_t n;

  snprintf(plugin_dir, sizeof(plugin_dir), "/tmp/hplip-arch-arm64-XXXXXX");
  if (mkdtemp(plugin_dir) == NULL) { perror("mkdtemp"); exit(2); }
  snprintf(plugin_tmp, sizeof(plugin_tmp), "%s/plugin_tmp", plugin_dir);
  if (mkdir(plugin_tmp, 0755) != 0) { perror("mkdir"); exit(2); }

  make_vendor_file(plugin_tmp, "hpcups-arm64.so");
  make_vendor_file(plugin_tmp, "hp-scan-x86_64.so");
  make_vendor_file(plugin_tmp, "hp-arm32.so");

  rc = hplip_stage_plugin_libs(plugin_dir, "arm64");
  check("arm64 release stages exactly its one library", rc == 1);

  n = readlink(plugin_tmp, target, sizeof(target) - 1);
  check("arm64 hpcups.so staged and points to its vendor file",
	n > 0 && strcmp(target, "hpcups.so") == 0);

  {
    char link[1024];
    snprintf(link, sizeof(link), "%s/hp-scan.so", plugin_tmp);
    if (lstat(link, &st) == 0)
      check("x86_64 library is not staged for arm64", 0);
    else
      check("x86_64 library is not staged for arm64", 1);
  }
}

// A vendor bundle which lacks this architecture entirely (the ARM-only-partial
// case) must report zero matches, not stage anything and not error.
static void
test_no_match(void)
{
  char plugin_dir[256];
  char plugin_tmp[1024];
  struct stat st;

  snprintf(plugin_dir, sizeof(plugin_dir), "/tmp/hplip-arch-none-XXXXXX");
  if (mkdtemp(plugin_dir) == NULL) { perror("mkdtemp"); exit(2); }
  snprintf(plugin_tmp, sizeof(plugin_tmp), "%s/plugin_tmp", plugin_dir);
  if (mkdir(plugin_tmp, 0755) != 0) { perror("mkdir"); exit(2); }

  make_vendor_file(plugin_tmp, "hpcups-arm64.so");

  if (hplip_stage_plugin_libs(plugin_dir, "x86_64") == 0)
    check("no x86_64 libraries yields a match count of 0", 1);
  else
    check("no x86_64 libraries yields a match count of 0", 0);

  {
    char link[1024];
    snprintf(link, sizeof(link), "%s/hpcups.so", plugin_tmp);
    if (lstat(link, &st) == 0)
      check("nothing is staged when nothing matches", 0);
    else
      check("nothing is staged when nothing matches", 1);
  }
}

int
main(void)
{
  test_x86_64();
  test_arm64();
  test_no_match();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
