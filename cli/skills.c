#define _GNU_SOURCE
#include "skills.h"
#include "log.h"
#include <cai/skills.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
static int fail(cai_error *error, const char *operation, const char *path) {
  char message[1024];
  snprintf(message, sizeof(message), "%s %s: %s", operation, path,
           strerror(errno));
  return cai_cli_error(error, CAI_ERR_TRANSPORT, message);
}
static int invalid(cai_error *error, const char *message) {
  return cai_cli_error(error, CAI_ERR_INVALID, message);
}
static void normalize_path(char *path) {
  char *read = path + 1, *write = path + 1, *end;
  size_t size;
  int last;
  while (*read) {
    while (*read == '/')
      read++;
    end = strchr(read, '/');
    if (end == NULL)
      end = read + strlen(read);
    last = *end == '\0';
    size = (size_t)(end - read);
    if (size == 2U && memcmp(read, "..", 2U) == 0) {
      if (write > path + 1) {
        write--;
        while (write > path + 1 && write[-1] != '/')
          write--;
      }
    } else if (size != 0U && !(size == 1U && *read == '.')) {
      memmove(write, read, size);
      write += size;
      *write++ = '/';
    }
    if (last)
      break;
    read = end + 1;
  }
  if (write > path + 1)
    write--;
  *write = '\0';
}
static int mkdir_root(char *path, cai_error *error) {
  char *p;
  struct stat st;
  int created;
  for (p = path + 1;; p++) {
    char saved = *p;
    if (saved != '/' && saved != '\0')
      continue;
    *p = '\0';
    created = mkdir(path, 0700) == 0;
    if ((!created && errno != EEXIST) || (created && chmod(path, 0700) != 0)) {
      *p = saved;
      return fail(error, "create skills directory", path);
    }
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
      *p = saved;
      return invalid(
          error,
          "skills directory parents must be real directories, not symlinks");
    }
    *p = saved;
    if (saved == '\0')
      break;
  }
  return CAI_OK;
}
static int root_path(const cai_cli_options *options, char *out,
                     cai_error *error) {
  const char *base;
  char relative[PATH_MAX], cwd[PATH_MAX];
  int n;
  if (options->skills_dir != NULL)
    n = snprintf(relative, sizeof(relative), "%s", options->skills_dir);
  else if (options->config_dir != NULL)
    n = snprintf(relative, sizeof(relative), "%s/skills", options->config_dir);
  else {
    base = getenv("XDG_CONFIG_HOME");
    if (base != NULL && base[0] != '\0')
      n = snprintf(relative, sizeof(relative), "%s/cai/skills", base);
    else {
      base = getenv("HOME");
      if (base == NULL || base[0] == '\0')
        return invalid(error,
                       "set HOME or XDG_CONFIG_HOME for the skills directory");
      n = snprintf(relative, sizeof(relative), "%s/.config/cai/skills", base);
    }
  }
  if (n < 0 || (size_t)n >= sizeof(relative))
    return invalid(error, "skills directory path exceeds PATH_MAX");
  if (relative[0] == '/')
    n = snprintf(out, PATH_MAX, "%s", relative);
  else {
    if (getcwd(cwd, sizeof(cwd)) == NULL)
      return fail(error, "resolve skills directory", relative);
    n = snprintf(out, PATH_MAX, "%s/%s", cwd, relative);
  }
  return n >= 0 && n < PATH_MAX
             ? CAI_OK
             : invalid(error, "skills directory path exceeds PATH_MAX");
}
static int metadata_at(int fd, const char *fallback, cai_skill_metadata **out,
                       cai_error *error) {
  FILE *file;
  cai_source *source = NULL;
  struct stat st;
  int resource, rc;
  resource =
      openat(fd, "SKILL.md", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (resource < 0)
    return fail(error, "open SKILL.md in", fallback);
  if (fstat(resource, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(resource);
    return invalid(error,
                   "SKILL.md must be a regular file, not a link/device/FIFO");
  }
  file = fdopen(resource, "rb");
  if (file == NULL) {
    close(resource);
    return fail(error, "read SKILL.md in", fallback);
  }
  rc = cai_source_file(file, 1, &source, error);
  if (rc != CAI_OK)
    fclose(file);
  else {
    rc = cai_skill_metadata_open(source, fallback, out, error);
    source->close(source);
  }
  return rc;
}
/* Both cleanup and copying stay below opened directories; symlinks are never
 * followed. Copy uses bounded buffers and preserves executable artifacts. */
static int remove_tree(int parent, const char *name) {
  int fd, rc = 0;
  DIR *directory;
  struct dirent *entry;
  struct stat st;
  if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? 0 : -1;
  if (!S_ISDIR(st.st_mode))
    return unlinkat(parent, name, 0);
  fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return -1;
  if (st.st_uid == geteuid() && fchmod(fd, 0700) != 0) {
    close(fd);
    return -1;
  }
  directory = fdopendir(fd);
  if (directory == NULL) {
    close(fd);
    return -1;
  }
  while ((entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (remove_tree(fd, entry->d_name) != 0)
      rc = -1;
  }
  closedir(directory);
  return rc == 0 ? unlinkat(parent, name, AT_REMOVEDIR) : rc;
}
static int copy_file(int src, int dst, const char *name, cai_error *error) {
  char bytes[65536];
  ssize_t n, written;
  size_t offset;
  while ((n = read(src, bytes, sizeof(bytes))) != 0) {
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return fail(error, "read skill artifact", name);
    }
    for (offset = 0U; offset < (size_t)n; offset += (size_t)written) {
      written = write(dst, bytes + offset, (size_t)n - offset);
      if (written < 0 && errno == EINTR) {
        written = 0;
        continue;
      }
      if (written <= 0)
        return fail(error, "write skill artifact", name);
    }
  }
  return CAI_OK;
}
static int copy_tree(int source, int destination, unsigned int depth,
                     cai_error *error) {
  DIR *directory;
  struct dirent *entry;
  struct stat st;
  int src, dst, rc = CAI_OK;
  if (depth > 128U)
    return invalid(error, "skill artifact tree exceeds 128 directory levels");
  directory = fdopendir(dup(source));
  if (directory == NULL)
    return fail(error, "list skill source", "directory");
  while (rc == CAI_OK) {
    errno = 0;
    entry = readdir(directory);
    if (entry == NULL) {
      if (errno != 0)
        rc = fail(error, "list skill source", "directory");
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (fstatat(source, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      rc = fail(error, "inspect skill artifact", entry->d_name);
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      if (mkdirat(destination, entry->d_name, 0700) != 0) {
        rc = fail(error, "create skill artifact directory", entry->d_name);
        break;
      }
      src = openat(source, entry->d_name,
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      dst = openat(destination, entry->d_name,
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (src < 0 || dst < 0 || fchmod(dst, 0700) != 0)
        rc = fail(error, "open skill artifact directory", entry->d_name);
      else
        rc = copy_tree(src, dst, depth + 1U, error);
      if (src >= 0)
        close(src);
      if (dst >= 0)
        close(dst);
    } else if (S_ISREG(st.st_mode)) {
      src = openat(source, entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      dst = openat(destination, entry->d_name,
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (src < 0 || dst < 0 ||
          fchmod(dst, 0600 | ((st.st_mode & 0111U) ? 0100 : 0)) != 0)
        rc = fail(error, "open skill artifact", entry->d_name);
      else
        rc = copy_file(src, dst, entry->d_name, error);
      if (src >= 0)
        close(src);
      if (dst >= 0 && close(dst) != 0 && rc == CAI_OK)
        rc = fail(error, "close skill artifact", entry->d_name);
    } else
      rc = invalid(error, "skill packages may contain only regular files and "
                          "directories; symlinks/devices/FIFOs are rejected");
  }
  closedir(directory);
  return rc;
}
static int check_existing_name(int fd, const char *name, const char *except,
                               unsigned int depth, cai_error *error) {
  DIR *directory;
  struct dirent *entry;
  struct stat st;
  cai_skill_metadata *metadata = NULL;
  cai_error ignored;
  int child, rc = CAI_OK;
  directory = fdopendir(dup(fd));
  if (directory == NULL)
    return fail(error, "list installed skills", "directory");
  while (rc == CAI_OK) {
    errno = 0;
    entry = readdir(directory);
    if (entry == NULL) {
      if (errno != 0)
        rc = fail(error, "list installed skills", "directory");
      break;
    }
    if (entry->d_name[0] == '.' ||
        (depth == 0U && except != NULL && strcmp(entry->d_name, except) == 0) ||
        fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(st.st_mode))
      continue;
    child = openat(fd, entry->d_name,
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0)
      continue;
    if (fstatat(child, "SKILL.md", &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(st.st_mode)) {
      cai_error_init(&ignored);
      if (metadata_at(child, entry->d_name, &metadata, &ignored) == CAI_OK &&
          strcmp(metadata->name(metadata), name) == 0)
        rc = invalid(error, "skill name already belongs to another installed "
                            "package directory");
      cai_skill_metadata_close(metadata);
      metadata = NULL;
      cai_error_cleanup(&ignored);
    } else if (depth < 8U)
      rc = check_existing_name(child, name, NULL, depth + 1U, error);
    close(child);
  }
  closedir(directory);
  return rc;
}
int cai_cli_install_skill(const cai_cli_options *options,
                          pslog_logger *logger) {
  cai_error error;
  cai_skill_metadata *metadata = NULL;
  struct stat st;
  char source_path[PATH_MAX], root[PATH_MAX], canonical[PATH_MAX],
      temporary[PATH_MAX];
  const char *name, *stage;
  int source = -1, parent = -1, destination = -1, rc, exists = 0;
  cai_error_init(&error);
  temporary[0] = '\0';
  if (lstat(options->install_skill, &st) != 0 || !S_ISDIR(st.st_mode)) {
    rc = invalid(
        &error,
        "--install-skill requires a real directory containing SKILL.md");
    goto done;
  }
  if (realpath(options->install_skill, source_path) == NULL) {
    rc = fail(&error, "resolve skill source", options->install_skill);
    goto done;
  }
  source = open(source_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (source < 0) {
    rc = fail(&error, "open skill source", source_path);
    goto done;
  }
  rc = metadata_at(source, source_path, &metadata, &error);
  if (rc != CAI_OK)
    goto done;
  name = metadata->name(metadata);
  if (name[0] == '.') {
    rc = invalid(&error, "installed skill names must not begin with '.'");
    goto done;
  }
  rc = root_path(options, root, &error);
  if (rc != CAI_OK)
    goto done;
  normalize_path(root);
  if (strncmp(root, source_path, strlen(source_path)) == 0 &&
      (root[strlen(source_path)] == '\0' || root[strlen(source_path)] == '/')) {
    rc = invalid(&error,
                 "skills destination must not be inside the source package");
    goto done;
  }
  rc = mkdir_root(root, &error);
  if (rc != CAI_OK)
    goto done;
  if (realpath(root, canonical) == NULL) {
    rc = fail(&error, "resolve skills directory", root);
    goto done;
  }
  if (snprintf(temporary, sizeof(temporary), "%s/%s", canonical, name) >=
      (int)sizeof(temporary)) {
    temporary[0] = '\0';
    rc = invalid(&error, "installed skill path exceeds PATH_MAX");
    goto done;
  }
  if (strcmp(temporary, source_path) == 0) {
    temporary[0] = '\0';
    rc = invalid(
        &error,
        "source and installed skill destination are the same directory");
    goto done;
  }
  temporary[0] = '\0';
  if (strncmp(canonical, source_path, strlen(source_path)) == 0 &&
      (canonical[strlen(source_path)] == '\0' ||
       canonical[strlen(source_path)] == '/')) {
    rc = invalid(&error,
                 "skills destination must not be inside the source package");
    goto done;
  }
  parent = open(canonical, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (parent < 0) {
    rc = fail(&error, "open skills directory", canonical);
    goto done;
  }
  if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
    exists = 1;
    if (!options->replace_skill) {
      rc = invalid(
          &error,
          "skill already exists; use --replace to replace it explicitly");
      goto done;
    }
    if (!S_ISDIR(st.st_mode)) {
      rc = invalid(&error,
                   "refusing to replace a non-directory skill destination");
      goto done;
    }
    destination =
        openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (destination < 0 ||
        fstatat(destination, "SKILL.md", &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(st.st_mode)) {
      rc = invalid(
          &error, "refusing to replace a directory without a regular SKILL.md");
      goto done;
    }
    close(destination);
    destination = -1;
  } else if (errno != ENOENT) {
    rc = fail(&error, "inspect installed skill", name);
    goto done;
  }
  rc = check_existing_name(parent, name, exists ? name : NULL, 0U, &error);
  if (rc != CAI_OK)
    goto done;
  if (snprintf(temporary, sizeof(temporary), "%s/.skill-install-XXXXXX",
               canonical) >= (int)sizeof(temporary) ||
      mkdtemp(temporary) == NULL) {
    temporary[0] = '\0';
    rc = fail(&error, "create skill installation staging directory", canonical);
    goto done;
  }
  stage = strrchr(temporary, '/') + 1;
  destination =
      openat(parent, stage, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (destination < 0 || fchmod(destination, 0700) != 0) {
    rc = fail(&error, "open skill staging directory", canonical);
    goto done;
  }
  rc = copy_tree(source, destination, 0U, &error);
  if (rc != CAI_OK)
    goto done;
  close(destination);
  destination = -1;
  if (renameat2(parent, stage, parent, name,
                exists ? RENAME_EXCHANGE : RENAME_NOREPLACE) != 0) {
    rc = fail(&error, "publish skill (atomic rename required)", name);
    goto done;
  }
  if (exists && remove_tree(parent, stage) != 0)
    cai_cli_log_message(logger, PSLOG_LEVEL_WARN, "skill_install_cleanup",
                        "diagnostic",
                        "Skill replaced; the previous package could not be "
                        "removed from the hidden staging directory.",
                        strlen("Skill replaced; the previous package could not "
                               "be removed from the hidden staging directory."),
                        NULL, 0U);
  temporary[0] = '\0';
  {
    pslog_field fields[3];
    fields[0] = pslog_str("skill", name);
    fields[1] = pslog_str("directory", canonical);
    fields[2] = pslog_bool("replaced", exists);
    cai_cli_log_message(logger, PSLOG_LEVEL_INFO, "skill_install", "diagnostic",
                        "Skill package installed",
                        strlen("Skill package installed"), fields, 3U);
  }
  printf("%s skill %s at %s/%s\n", exists ? "Replaced" : "Installed", name,
         canonical, name);
  rc = CAI_OK;
done:
  if (destination >= 0)
    close(destination);
  if (temporary[0] != '\0' && parent >= 0)
    (void)remove_tree(parent, strrchr(temporary, '/') + 1);
  if (source >= 0)
    close(source);
  if (parent >= 0)
    close(parent);
  if (rc != CAI_OK)
    cai_cli_log_error(logger, "install skill", &error);
  cai_skill_metadata_close(metadata);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}
