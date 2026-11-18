// SPDX-License-Identifier: Apache-2.0
// webc/upload.h — the disk half. Two functions and no policy: what may be written, and under
// what name, is decided in the header where the app can read it.
// System headers first: the shelf's `select()` and `state()` are macros, and a system header
// included after them is a header in the wrong language. Same rule the reference states.
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <webc/upload.h>

bool wc_upload_mkdir_p(const char *dir) {
  if (!dir || !*dir) return true;
  char path[4096];
  size_t n = strlen(dir);
  if (n >= sizeof path) return false;
  memcpy(path, dir, n + 1);
  struct stat st;
  if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
  for (size_t i = 1; i <= n; i++) {
    if (path[i] != '/' && path[i] != 0) continue;
    char keep = path[i];
    path[i] = 0;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
      path[i] = keep;
      return false;
    }
    path[i] = keep;
  }
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool wc_upload_write(const char *path, str data, i32 mode) {
  if (!path || !*path) return false;
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) return false;
  size_t off = 0;
  while (off < data.n) {
    ssize_t w = write(fd, data.p + off, data.n - off);
    if (w <= 0) {
      close(fd);
      unlink(path); // a half-written file is worse than none
      return false;
    }
    off += (size_t)w;
  }
  close(fd);
  return true;
}

// One decimal place and the unit a person says. A refusal message that says "3145728" is a
// message nobody reads.
str wc_upload_bytes_text(size_t n) {
  Buf b;
  buf_init(&b, wc_arena_get(), 16);
  if (n < 1024) {
    buf_printf(&b, "%zu B", n);
  } else if (n < 1024 * 1024) {
    buf_printf(&b, "%.1f KB", (double)n / 1024.0);
  } else if (n < (size_t)1024 * 1024 * 1024) {
    buf_printf(&b, "%.1f MB", (double)n / (1024.0 * 1024.0));
  } else {
    buf_printf(&b, "%.1f GB", (double)n / (1024.0 * 1024.0 * 1024.0));
  }
  return buf_str(&b);
}
