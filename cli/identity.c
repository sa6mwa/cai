#define _POSIX_C_SOURCE 200809L
#include "identity.h"
#include "pouch_stream.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int cai_cli_remove_at(int parent, const char *name) {
  struct stat st;
  struct dirent *entry;
  DIR *directory;
  int fd, saved = 0;
  if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(st.st_mode))
    return unlinkat(parent, name, 0);
  fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return -1;
  if (fchmod(fd, 0700) != 0 || (directory = fdopendir(fd)) == NULL) {
    saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  errno = 0;
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (cai_cli_remove_at(fd, entry->d_name) != 0 && saved == 0)
      saved = errno;
    errno = 0;
  }
  if (errno != 0 && saved == 0)
    saved = errno;
  if (closedir(directory) != 0 && saved == 0)
    saved = errno;
  if (saved != 0) {
    errno = saved;
    return -1;
  }
  return unlinkat(parent, name, AT_REMOVEDIR);
}

int cai_cli_identity_valid(const char *name) {
  return name != NULL && name[0] >= 'a' && name[0] <= 'z' &&
         strlen(name) <= 32U &&
         strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(name);
}
int cai_cli_environment_prefix(const cai_app_config *app, char out[65],
                               cai_error *error) {
  const char *name = app != NULL && app->program_name != NULL
                         ? app->program_name
                         : CAI_APP_DEFAULT_PROGRAM_NAME;
  size_t i, n;
  if (!cai_cli_identity_valid(name))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "invalid application program_name");
  if (app != NULL && app->environment_prefix != NULL) {
    name = app->environment_prefix;
    n = strlen(name);
    if (n == 0U || n > 64U || name[n - 1U] != '_' ||
        (name[0] >= '0' && name[0] <= '9') ||
        strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz01234"
                     "56789_") != n)
      return cai_cli_error(
          error, CAI_ERR_INVALID,
          "environment_prefix must be a variable prefix ending in '_'");
    strcpy(out, name);
  } else {
    n = strlen(name);
    for (i = 0U; i < n; i++)
      out[i] = name[i] == '-'                     ? '_'
               : name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 'a' + 'A')
                                                  : name[i];
    out[n++] = '_';
    out[n] = '\0';
  }
  return CAI_OK;
}
int cai_cli_xdg_path(char *out, size_t size, const char *variable,
                     const char *fallback, const char *name, cai_error *error) {
  const char *base = getenv(variable);
  int n;
  if (base != NULL && base[0] == '/')
    n = snprintf(out, size, "%s/%s", base, name);
  else {
    base = getenv("HOME");
    if (base == NULL || base[0] != '/')
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "set HOME or an absolute XDG root");
    n = snprintf(out, size, "%s/%s/%s", base, fallback, name);
  }
  return n < 0 || (size_t)n >= size
             ? cai_cli_error(error, CAI_ERR_INVALID,
                             "application path is too long")
             : CAI_OK;
}
int cai_cli_private_directory(const char *path, cai_error *error) {
  char copy[4096], *cursor;
  struct stat status;
  if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(copy))
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "private directory must be an absolute path");
  strcpy(copy, path);
  for (cursor = copy + 1;; cursor++) {
    if (*cursor == '/' || *cursor == '\0') {
      char saved = *cursor;
      *cursor = '\0';
      if (mkdir(copy, 0700) != 0 && errno != EEXIST)
        return cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "create private application directory");
      *cursor = saved;
      if (saved == '\0')
        break;
    }
  }
  if (lstat(copy, &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 077U) != 0U)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "application directory must be owned by you with mode "
                         "0700 and must not be a symlink");
  return CAI_OK;
}
