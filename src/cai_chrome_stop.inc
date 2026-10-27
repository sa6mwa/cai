/* Removal uses a separate socket, so an occupied browser connection cannot
 * prevent shutdown. The host matches its startup executable before stopping. */
static int chrome_stop_one(const char *path, const char *executable,
                           int *stopped, int *unavailable, cai_error *error) {
  cai_chrome control;
  cai_chrome_stop_request request;
  char *params, *reply = NULL;
  int rc;
  memset(&control, 0, sizeof(control));
  control.fd = -1;
  control.timeout_ms = CAI_CHROME_CLEANUP_TIMEOUT_MS + 1500L;
  control.limit = 32768U;
  *unavailable = 0;
  errno = 0;
  rc = cai_chrome_connect_socket(path, &control.fd, error);
  if (rc != CAI_OK) {
    *unavailable = errno == ENOENT || errno == ECONNREFUSED;
    return rc;
  }
  request.executable = executable;
  params = cai_chrome_serialize(&cai_chrome_stop_map, &request, error);
  rc = params != NULL
           ? cai_chrome_request(&control, "shutdown", params, &reply, error)
           : CAI_ERR_NOMEM;
  if (rc == CAI_OK) {
    if (strcmp(reply, "\"stopped\"") == 0)
      (*stopped)++;
    else if (strcmp(reply, "\"different-executable\"") != 0)
      rc = cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "Chrome host could not finish shutdown", path);
  }
  if (control.fd >= 0)
    close(control.fd);
  cai_free_mem(NULL, params);
  cai_free_mem(NULL, reply);
  return rc;
}

static int chrome_old_host_owned(int fd, const char *executable,
                                 cai_error *error) {
  struct ucred peer;
  socklen_t length = sizeof(peer);
  char proc[64], path[4096];
  ssize_t n;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Cannot identify old Chrome host peer");
  snprintf(proc, sizeof(proc), "/proc/%ld/exe", (long)peer.pid);
  n = readlink(proc, path, sizeof(path) - 1U);
  if (n <= 0 || (size_t)n >= sizeof(path) - 1U)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Cannot identify old Chrome host executable");
  path[n] = '\0';
  if (strcmp(path, executable) == 0 ||
      (strncmp(path, executable, strlen(executable)) == 0 &&
       strcmp(path + strlen(executable), " (deleted)") == 0))
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Chrome host has no shutdown endpoint; reload the "
                         "ChatGPT extension or restart Chrome once to replace "
                         "the old host");
  return CAI_OK;
}

static int chrome_missing_control(const char *path, const char *executable,
                                  cai_error *error) {
  int fd = -1, rc;
  errno = 0;
  rc = cai_chrome_connect_socket(path, &fd, error);
  if (fd >= 0) {
    rc = chrome_old_host_owned(fd, executable, error);
    close(fd);
  } else if (errno == ENOENT || errno == ECONNREFUSED) {
    cai_error_cleanup(error);
    cai_error_init(error);
    rc = CAI_OK;
  }
  return rc;
}

static int chrome_stop_hosts(const char *selected, pslog_logger *logger,
                             cai_error *error) {
  char directory[4096], executable[4096], path[4096], control[4096];
  DIR *entries;
  struct dirent *entry;
  struct stat st;
  ssize_t n;
  size_t length;
  int rc, result = CAI_OK, stopped = 0, unavailable;
  cai_error local;
  if (selected == NULL) {
    const char *root = getenv("XDG_RUNTIME_DIR");
    const char *suffix = "/cai/chrome";
    if (root == NULL || root[0] == '\0') {
      root = getenv("XDG_CACHE_HOME");
      if (root == NULL || root[0] == '\0') {
        root = getenv("HOME");
        suffix = "/.cache/cai/chrome";
      }
    }
    if (root == NULL || root[0] != '/' ||
        snprintf(directory, sizeof(directory), "%s%s", root, suffix) >=
            (int)sizeof(directory))
      return cai_set_error(error, CAI_ERR_INVALID,
                           "Cannot resolve the Chrome host directory");
  } else {
    if (strlen(selected) >= sizeof(directory))
      return cai_set_error(error, CAI_ERR_INVALID,
                           "Chrome host directory is too long");
    strcpy(directory, selected);
  }
  if (lstat(directory, &st) != 0 && errno == ENOENT)
    return CAI_OK;
  rc = cai_chrome_private_directory(directory, error);
  if (rc != CAI_OK)
    return rc;
  n = readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
  if (n <= 0 || (size_t)n >= sizeof(executable) - 1U)
    return cai_set_error(error, CAI_ERR_TRANSPORT,
                         "Cannot identify the Chrome host executable");
  executable[n] = '\0';
  entries = opendir(directory);
  if (entries == NULL)
    return cai_set_error_detail(error, CAI_ERR_TRANSPORT,
                                "Cannot enumerate Chrome hosts", directory);
  cai_error_init(&local);
  while ((entry = readdir(entries)) != NULL) {
    length = strlen(entry->d_name);
    if (length < 6U || strcmp(entry->d_name + length - 5U, ".sock") != 0)
      continue;
    if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >=
            (int)sizeof(path) ||
        snprintf(control, sizeof(control), "%s/%.*s.ctl", directory,
                 (int)(length - 5U), entry->d_name) >= (int)sizeof(control)) {
      rc = cai_set_error(&local, CAI_ERR_INVALID,
                         "Chrome host path is too long");
    } else if (lstat(control, &st) != 0 && errno == ENOENT) {
      /* Detect live older hosts; never claim that they stopped. */
      rc = chrome_missing_control(path, executable, &local);
    } else {
      rc = chrome_stop_one(control, executable, &stopped, &unavailable, &local);
      if (unavailable) {
        cai_error_cleanup(&local);
        cai_error_init(&local);
        rc = chrome_missing_control(path, executable, &local);
      }
    }
    if (rc != CAI_OK) {
      if (logger != NULL)
        logger->errorf(logger, "Chrome host shutdown failed",
                       "socket=%s error=%s", path,
                       local.message != NULL ? local.message : "failed");
      if (result == CAI_OK) {
        result = rc;
        if (error != NULL) {
          cai_error_cleanup(error);
          *error = local;
          cai_error_init(&local);
        }
      }
    }
    cai_error_cleanup(&local);
    cai_error_init(&local);
  }
  closedir(entries);
  cai_error_cleanup(&local);
  if (stopped > 0)
    printf("Stopped %d Chrome native host(s).\n", stopped);
  return result;
}

int cai_chrome_native_host_setup_at(const char *program_name,
                                    const char *manifest, int remove,
                                    const char *directory, pslog_logger *logger,
                                    cai_error *error) {
  int rc = chrome_registration(program_name, manifest, remove, logger, error);
  if (rc == CAI_OK && remove)
    rc = chrome_stop_hosts(directory, logger, error);
  return rc;
}

int cai_chrome_native_host_setup_named(const char *program_name,
                                       const char *manifest, int remove,
                                       pslog_logger *logger, cai_error *error) {
  return cai_chrome_native_host_setup_at(program_name, manifest, remove, NULL,
                                         logger, error);
}
