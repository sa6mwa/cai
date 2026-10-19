#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "cai_chrome_host.h"
#include "cai_chrome_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOST_NAME "com.openai.codexextension"
#define EXTENSION_ORIGIN "chrome-extension://hehggadaopoacecdllhhajmbjkdcmajg/"

int cai_cli_chrome_check_system_registration(const char *const *paths,
                                             size_t count, cai_error *error) {
  size_t i;
  struct stat st;
  for (i = 0U; i < count; i++) {
    if (lstat(paths[i], &st) == 0)
      return cai_set_error_detail(
          error, CAI_ERR_INVALID,
          "A system Chrome native host is registered; CAI will not shadow it. "
          "Keep that registration, or deliberately remove it before setup",
          paths[i]);
    if (errno != ENOENT)
      return cai_set_error_detail(
          error, CAI_ERR_TRANSPORT,
          "Cannot inspect the system Chrome registration; setup was stopped",
          paths[i]);
  }
  return CAI_OK;
}

typedef struct chrome_manifest {
  const char *name;
  const char *description;
  const char *path;
  const char *type;
  lonejson_string_array allowed_origins;
} chrome_manifest;
static const lonejson_field manifest_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_manifest, name, "name"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_manifest, description,
                                    "description"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_manifest, path, "path"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_manifest, type, "type"),
    LONEJSON_FIELD_STRING_ARRAY(chrome_manifest, allowed_origins,
                                "allowed_origins", LONEJSON_OVERFLOW_FAIL)};
LONEJSON_MAP_DEFINE(manifest_map, chrome_manifest, manifest_fields);

/* Walk from / with openat: refuse symlink components and create only missing
 * directories. Existing browser configuration need not have private mode. */
static int open_parent(const char *path, int create, char *leaf, size_t size,
                       cai_error *error) {
  char copy[4096];
  char *cursor, *next;
  int fd, child;
  struct stat st;
  if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(copy)) {
    cai_set_error(error, CAI_ERR_INVALID,
                  "Chrome configuration path must be absolute");
    return -1;
  }
  strcpy(copy, path);
  cursor = copy + 1;
  fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  while ((next = strchr(cursor, '/')) != NULL) {
    *next = '\0';
    if (cursor[0] == '\0' || strcmp(cursor, ".") == 0 ||
        strcmp(cursor, "..") == 0)
      goto invalid;
    child = openat(fd, cursor, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0 && errno == ENOENT && create) {
      if (mkdirat(fd, cursor, 0700) != 0 && errno != EEXIST)
        goto failure;
      child =
          openat(fd, cursor, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (child < 0) {
      if (!create && errno == ENOENT) {
        close(fd);
        return -2;
      }
      goto failure;
    }
    if (fstat(child, &st) != 0 || (st.st_uid != 0 && st.st_uid != geteuid()) ||
        (st.st_mode & 002U) != 0U) {
      close(child);
      goto invalid;
    }
    close(fd);
    fd = child;
    cursor = next + 1;
  }
  if (cursor[0] == '\0' || strlen(cursor) >= size || strcmp(cursor, ".") == 0 ||
      strcmp(cursor, "..") == 0)
    goto invalid;
  strcpy(leaf, cursor);
  return fd;
invalid:
  cai_set_error(error, CAI_ERR_INVALID,
                "Chrome configuration path contains an unsafe component");
  close(fd);
  return -1;
failure:
  cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                       "Cannot open Chrome configuration directory",
                       strerror(errno));
  close(fd);
  return -1;
}
int cai_cli_chrome_directory(char *out, size_t capacity, int create,
                             cai_error *error) {
  const char *root;
  const char *suffix;
  char leaf[100];
  int fd;
  int n;
  root = getenv("XDG_RUNTIME_DIR");
  suffix = "/cai/chrome";
  if (root == NULL || root[0] == '\0') {
    root = getenv("XDG_CACHE_HOME");
    if (root == NULL || root[0] == '\0') {
      root = getenv("HOME");
      suffix = "/.cache/cai/chrome";
    }
  }
  if (root == NULL || root[0] != '/')
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "Chrome requires an absolute XDG_RUNTIME_DIR, XDG_CACHE_HOME, or HOME");
  n = snprintf(out, capacity, "%s%s", root, suffix);
  if (n < 0 || (size_t)n >= capacity)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Chrome socket directory path is too long");
  if (create) {
    char child[4096];
    if (snprintf(child, sizeof(child), "%s/unused", out) >= (int)sizeof(child))
      return cai_set_error(error, CAI_ERR_INVALID,
                           "Chrome socket directory path is too long");
    fd = open_parent(child, 1, leaf, sizeof(leaf), error);
    if (fd < 0)
      return error != NULL ? error->code : CAI_ERR_TRANSPORT;
    close(fd);
  }
  if (!create) {
    struct stat st;
    if (lstat(out, &st) != 0 && errno == ENOENT)
      return cai_set_error(
          error, CAI_ERR_INVALID,
          "No CAI Chrome host is connected. Run cai --chrome-setup, then open "
          "the ChatGPT extension in Chrome.");
  }
  return cai_chrome_private_directory(out, error);
}
int cai_chrome_native_host_setup(const char *override, int remove,
                                 pslog_logger *logger, cai_error *error) {
  char manifest_path[4096], executable[4096], leaf[200], temporary[200];
  const char *root;
  const char *suffix;
  ssize_t n;
  int parent, fd, rc = CAI_OK;
  struct stat st, current;
  chrome_manifest existing, wanted;
  char contents[16385];
  char *json = NULL;
  lonejson_error je;
  size_t used = 0U, length, offset;
  memset(&existing, 0, sizeof(existing));
  memset(&wanted, 0, sizeof(wanted));
  lonejson_error_init(&je);
  n = readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
  if (n < 0 || (size_t)n >= sizeof(executable) - 1U)
    return cai_set_error(
        error, CAI_ERR_TRANSPORT,
        "Cannot resolve the cai executable for Chrome registration");
  executable[n] = '\0';
  if (strstr(executable, " (deleted)") != NULL)
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "Reinstall cai before registering its deleted executable");
  if (override != NULL) {
    if (strlen(override) >= sizeof(manifest_path))
      return cai_set_error(error, CAI_ERR_INVALID,
                           "Chrome manifest path is too long");
    strcpy(manifest_path, override);
  } else {
    root = getenv("CHROME_CONFIG_HOME");
    suffix = "/google-chrome/NativeMessagingHosts/" HOST_NAME ".json";
    if (root == NULL || root[0] == '\0')
      root = getenv("XDG_CONFIG_HOME");
    if (root == NULL || root[0] == '\0') {
      root = getenv("HOME");
      suffix = "/.config/google-chrome/NativeMessagingHosts/" HOST_NAME ".json";
    }
    if (root == NULL || root[0] != '/' ||
        snprintf(manifest_path, sizeof(manifest_path), "%s%s", root, suffix) >=
            (int)sizeof(manifest_path))
      return cai_set_error(error, CAI_ERR_INVALID,
                           "Chrome manifest requires an absolute config home");
  }
  parent = open_parent(manifest_path, !remove, leaf, sizeof(leaf), error);
  if (parent == -2 && remove) {
    printf("No Chrome registration at %s\n", manifest_path);
    return CAI_OK;
  }
  if (parent < 0)
    return error != NULL ? error->code : CAI_ERR_TRANSPORT;
  if (fstat(parent, &st) != 0 || (st.st_mode & 022U) != 0U) {
    close(parent);
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "Chrome native-host directory must not be writable by others");
  }
  if (strcmp(leaf, HOST_NAME ".json") != 0) {
    close(parent);
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "Chrome manifest must be named com.openai.codexextension.json");
  }
  fd = openat(parent, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd >= 0) {
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 022U) || st.st_size > 16384) {
      close(fd);
      close(parent);
      return cai_set_error_detail(
          error, CAI_ERR_INVALID,
          "Chrome host registration is unsafe; it was left unchanged",
          manifest_path);
    }
    while (used < sizeof(contents) - 1U &&
           (n = read(fd, contents + used, sizeof(contents) - 1U - used)) > 0)
      used += (size_t)n;
    close(fd);
    contents[used] = '\0';
    if (n < 0 ||
        CAI_LJ_CHROME->parse_cstr(CAI_LJ_CHROME, &manifest_map, &existing,
                                  contents, &je) != LONEJSON_STATUS_OK ||
        existing.name == NULL || existing.path == NULL ||
        existing.type == NULL || existing.description == NULL ||
        strcmp(existing.name, HOST_NAME) != 0 ||
        strcmp(existing.path, executable) != 0 ||
        strcmp(existing.type, "stdio") != 0 ||
        strcmp(existing.description, "CAI ChatGPT Chrome bridge") != 0 ||
        existing.allowed_origins.count != 1U ||
        existing.allowed_origins.items == NULL ||
        existing.allowed_origins.items[0] == NULL ||
        strcmp(existing.allowed_origins.items[0], EXTENSION_ORIGIN) != 0) {
      char detail[4600];
      snprintf(detail, sizeof(detail),
               "%s%s%s. CAI will not overwrite or remove it. Keep this "
               "registration, or deliberately remove it before running "
               "--chrome-setup.",
               manifest_path, existing.path != NULL ? " points to " : "",
               existing.path != NULL ? existing.path : "");
      rc = cai_set_error_detail(
          error, CAI_ERR_INVALID,
          "Another or unrecognized Chrome native host is registered", detail);
    } else if (remove) {
      /* Refuse a concurrent replacement between validation and unlink. */
      if (fstatat(parent, leaf, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
          current.st_ino != st.st_ino || current.st_dev != st.st_dev ||
          unlinkat(parent, leaf, 0) != 0)
        rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                           "Chrome registration changed during removal; "
                           "inspect it before retrying");
      else {
        (void)fsync(parent);
        printf("Removed CAI Chrome registration: %s\n", manifest_path);
      }
    } else
      printf("CAI Chrome registration already installed: %s\n", manifest_path);
    CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &manifest_map, &existing);
  } else if (errno != ENOENT) {
    rc = cai_set_error_detail(
        error, CAI_ERR_TRANSPORT,
        "Cannot inspect Chrome registration; it was left unchanged",
        strerror(errno));
  } else if (remove) {
    printf("No Chrome registration at %s\n", manifest_path);
  } else {
    static const char *const system_paths[] = {
        "/etc/opt/chrome/native-messaging-hosts/" HOST_NAME ".json",
        "/etc/chromium/native-messaging-hosts/" HOST_NAME ".json"};
    char *origins[1];
    rc = cai_cli_chrome_check_system_registration(
        system_paths, sizeof(system_paths) / sizeof(system_paths[0]), error);
    if (rc != CAI_OK) {
      close(parent);
      return rc;
    }
    origins[0] = cai_strdup(NULL, EXTENSION_ORIGIN);
    if (origins[0] == NULL) {
      close(parent);
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "Cannot allocate extension origin");
    }
    wanted.name = HOST_NAME;
    wanted.description = "CAI ChatGPT Chrome bridge";
    wanted.path = executable;
    wanted.type = "stdio";
    wanted.allowed_origins.items = origins;
    wanted.allowed_origins.count = 1U;
    json = cai_chrome_serialize(&manifest_map, &wanted, error);
    cai_free_mem(NULL, origins[0]);
    if (json == NULL)
      rc = CAI_ERR_NOMEM;
    else {
      /* Publish only after a complete fsynced write, without replacing a
       * competing installer. */
      snprintf(temporary, sizeof(temporary), ".cai-chrome-%ld.tmp",
               (long)getpid());
      fd = openat(parent, temporary,
                  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd < 0)
        rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                  "Cannot create Chrome registration without "
                                  "overwriting another host",
                                  strerror(errno));
      else {
        length = strlen(json);
        offset = 0U;
        while (offset < length) {
          n = write(fd, json + offset, length - offset);
          if (n < 0 && errno == EINTR)
            continue;
          if (n <= 0) {
            rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                      "Cannot write Chrome registration",
                                      strerror(errno));
            break;
          }
          offset += (size_t)n;
        }
        if (rc == CAI_OK && fsync(fd) != 0)
          rc = cai_set_error(error, CAI_ERR_TRANSPORT,
                             "Cannot sync Chrome registration");
        close(fd);
        if (rc == CAI_OK && linkat(parent, temporary, parent, leaf, 0) != 0)
          rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                    "Another installer created the Chrome "
                                    "registration; it was left unchanged",
                                    strerror(errno));
        (void)unlinkat(parent, temporary, 0);
        if (rc == CAI_OK) {
          (void)fsync(parent);
          printf("Installed CAI Chrome registration: %s\nOpen the ChatGPT "
                 "extension in Chrome, then run cai --chrome.\n",
                 manifest_path);
        }
      }
    }
    cai_free_mem(NULL, json);
  }
  close(parent);
  if (logger != NULL)
    logger->debugf(logger, "Chrome registration command finished",
                   "path=%s remove=%b result=%d", manifest_path, remove, rc);
  return rc;
}
