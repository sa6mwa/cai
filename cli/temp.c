#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "temp.h"
#include "identity.h"
#include "pouch_stream.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void cai_cli_temp_init(cai_cli_temp *temp) {
  memset(temp, 0, sizeof(*temp));
  temp->parent_fd = temp->root_fd = temp->cwd_fd = -1;
}
static int path(char *out, const char *base, const char *suffix,
                cai_error *error) {
  int n = snprintf(out, PATH_MAX, "%s/%s", base, suffix);
  return n >= 0 && n < PATH_MAX
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_INVALID,
                             "temporary workspace path is too long");
}
int cai_cli_temp_open(cai_cli_temp *temp, cai_cli_options *options,
                      cai_error *error) {
  char parent[PATH_MAX];
  int rc;
  rc = path(parent, options->cache_dir, "temp", error);
  if (rc == CAI_OK)
    rc = cai_cli_private_directory(parent, error);
  if (rc != CAI_OK)
    return rc;
  temp->parent_fd =
      open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  temp->cwd_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (temp->parent_fd < 0 || temp->cwd_fd < 0)
    return cai_cli_error(
        error, CAI_ERR_TRANSPORT,
        "open temporary workspace parent or original directory");
  if (realpath(parent, temp->root) == NULL)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "resolve temporary cache");
  strcpy(parent, temp->root);
  rc = path(temp->root, parent, "chat-XXXXXX", error);
  if (rc != CAI_OK)
    return rc;
  if (mkdtemp(temp->root) == NULL) {
    temp->root[0] = '\0';
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create temporary workspace");
  }
  temp->root_fd =
      open(temp->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (temp->root_fd < 0 || mkdirat(temp->root_fd, "workspace", 0700) != 0 ||
      mkdirat(temp->root_fd, "private", 0700) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "create temporary directories");
#define PATH(member, base, suffix)                                             \
  if ((rc = path(temp->member, temp->base, suffix, error)) != CAI_OK)          \
  return rc
  PATH(workspace, root, "workspace");
  PATH(private_dir, root, "private");
  PATH(state, private_dir, "state");
  PATH(cache, private_dir, "cache");
  PATH(data, private_dir, "data");
  PATH(history, state, "history");
  PATH(clipboard, cache, "clipboard");
  PATH(logs, cache, "logs");
  PATH(key, state, "pouch.key");
  PATH(exports, data, "exports");
#undef PATH
  if (chdir(temp->workspace) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "enter temporary workspace");
  options->state_dir = temp->state;
  options->cache_dir = temp->cache;
  options->data_dir = temp->data;
  options->history_dir = temp->history;
  options->clipboard_dir = temp->clipboard;
  options->log_dir = temp->logs;
  options->pouch_key_file = temp->key;
  options->export_dir = temp->exports;
  options->lockd = options->lockd_client_pem = NULL;
  options->new_session = 1;
  return CAI_OK;
}

static int confirm(const char *workspace, int stop_fd, int (*stop)(void *),
                   void *context) {
  struct pollfd fds[2];
  char answer[16], ch;
  size_t length = 0U;
  int overflow = 0, rc;
  if (fprintf(stdout, "Remove temporary workspace %s? [y/N] ", workspace) < 0 ||
      fflush(stdout) != 0)
    return 0;
  memset(fds, 0, sizeof(fds));
  fds[0].fd = STDIN_FILENO;
  fds[1].fd = stop_fd;
  fds[0].events = fds[1].events = POLLIN;
  for (;;) {
    rc = poll(fds, stop_fd >= 0 ? 2U : 1U, -1);
    if (rc < 0 || (stop != NULL && stop(context)) ||
        (fds[0].revents & (POLLERR | POLLNVAL)) != 0)
      return 0;
    if ((fds[0].revents & (POLLIN | POLLHUP)) == 0)
      continue;
    if (read(STDIN_FILENO, &ch, 1U) != 1)
      return 0;
    if (ch == '\n' || ch == '\r') {
      answer[length] = '\0';
      if (!overflow && (strcmp(answer, "y") == 0 || strcmp(answer, "yes") == 0))
        return 1;
      return 0;
    }
    if (ch >= 'A' && ch <= 'Z')
      ch = (char)(ch - 'A' + 'a');
    if (length + 1U < sizeof(answer))
      answer[length++] = ch;
    else
      overflow = 1;
  }
}

int cai_cli_temp_close(cai_cli_temp *temp, int force, int ask, int stop_fd,
                       int (*stop)(void *), void *context, int *kept,
                       cai_error *error) {
  struct stat opened, named;
  const char *name;
  int remove_workspace = force, rc = CAI_OK;
  *kept = 0;
  if (temp->cwd_fd >= 0 && fchdir(temp->cwd_fd) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "restore original directory");
  if (temp->root_fd >= 0) {
    name = strrchr(temp->root, '/') + 1U;
    if (fstat(temp->root_fd, &opened) != 0 ||
        fstatat(temp->parent_fd, name, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino)
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "temporary root changed; automatic deletion refused");
    if (!force && ask && rc == CAI_OK && !(stop != NULL && stop(context)))
      remove_workspace = confirm(temp->workspace, stop_fd, stop, context);
    if (cai_cli_remove_at(temp->root_fd, "private") != 0)
      rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                         "remove temporary private history and state");
    if (rc == CAI_OK && remove_workspace) {
      if (cai_cli_remove_at(temp->root_fd, "workspace") != 0 ||
          unlinkat(temp->parent_fd, name, AT_REMOVEDIR) != 0)
        rc = cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "remove temporary workspace");
    } else
      *kept = 1;
  }
  if (temp->cwd_fd >= 0)
    close(temp->cwd_fd);
  if (temp->root_fd >= 0)
    close(temp->root_fd);
  if (temp->parent_fd >= 0)
    close(temp->parent_fd);
  temp->cwd_fd = temp->root_fd = temp->parent_fd = -1;
  return rc;
}
