/* Pouch is currently the only history backend. Native shared/exclusive locks
 * coordinate app runtime use and deletion across processes on that root.
 * The namespace registry serializes lock-file creation/removal, preventing
 * stale-inode races without retaining per-conversation files after deletion.
 * No lock is inherited by executed commands; process exit releases use. */
typedef struct cai_cli_session_guard {
  int directory, registry, session;
  char id[CAI_AGENT_SESSION_ID_MAX];
} cai_cli_session_guard;

static int session_component(const char *name) {
  const unsigned char *p = (const unsigned char *)name;
  if (p == NULL || *p == '\0' || strcmp(name, ".") == 0 ||
      strcmp(name, "..") == 0)
    return 0;
  for (; *p != '\0'; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
      return 0;
  return 1;
}
static int session_private_fd(int fd, int directory) {
  struct stat st;
  return fd >= 0 && fstat(fd, &st) == 0 && st.st_uid == geteuid() &&
         (st.st_mode & 077U) == 0U &&
         (directory ? S_ISDIR(st.st_mode)
                    : S_ISREG(st.st_mode) && st.st_nlink == 1);
}
static int session_private_directory(int parent, const char *name) {
  int fd;
  if (mkdirat(parent, name, 0700) != 0 && errno != EEXIST)
    return -1;
  fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (!session_private_fd(fd, 1)) {
    if (fd >= 0)
      close(fd);
    return -1;
  }
  return fd;
}
static void session_guard_close(cai_cli_session_guard *guard) {
  if (guard == NULL)
    return;
  if (guard->session >= 0)
    close(guard->session);
  if (guard->registry >= 0)
    close(guard->registry);
  if (guard->directory >= 0)
    close(guard->directory);
  free(guard);
}
static int session_guard_open(cai_cli_pouch *pouch, const char *id,
                              int exclusive, cai_cli_session_guard **out,
                              int *busy, cai_error *error) {
  cai_cli_session_guard *guard;
  char namespace_key[65];
  int root, directory, rc = CAI_OK;
  *out = NULL;
  if (busy != NULL)
    *busy = 0;
  if (!session_component(id) || strlen(id) >= CAI_AGENT_SESSION_ID_MAX)
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "unsafe conversation identity");
  if (pouch->root_directory[0] == '\0')
    return cai_cli_error(error, CAI_ERR_INVALID,
                         "Conversation use and deletion require Pouch");
  rc = latest_key(pouch->sessions_namespace, namespace_key, error);
  if (rc != CAI_OK)
    return rc;
  guard = calloc(1U, sizeof(*guard));
  if (guard == NULL)
    return cai_cli_error(error, CAI_ERR_NOMEM, "allocate conversation lock");
  guard->directory = guard->registry = guard->session = -1;
  strcpy(guard->id, id);
  root = open(pouch->root_directory,
              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  directory = root < 0 ? -1 : session_private_directory(root, ".session-locks");
  if (root >= 0)
    close(root);
  if (directory >= 0) {
    guard->directory = session_private_directory(directory, namespace_key);
    close(directory);
  }
  if (guard->directory >= 0)
    guard->registry = openat(guard->directory, ".registry",
                             O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (!session_private_fd(guard->registry, 0) ||
      flock(guard->registry, LOCK_EX) != 0)
    rc = cai_cli_error(error, CAI_ERR_TRANSPORT, "lock conversation registry");
  if (rc == CAI_OK) {
    guard->session = openat(guard->directory, id,
                            O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (!session_private_fd(guard->session, 0))
      rc = cai_cli_error(error, CAI_ERR_INVALID,
                         "conversation lock must be a private regular file");
    else if (flock(guard->session, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) !=
             0) {
      int unavailable = errno == EWOULDBLOCK || errno == EAGAIN;
      if (busy != NULL)
        *busy = unavailable;
      rc = cai_cli_error(
          error, unavailable ? CAI_ERR_INVALID : CAI_ERR_TRANSPORT,
          "Conversation is currently open; close it before deleting");
    }
  }
  if (rc == CAI_OK) {
    if (!exclusive) {
      close(guard->registry);
      guard->registry = -1;
    }
    *out = guard;
  } else
    session_guard_close(guard);
  return rc;
}
void cai_cli_pouch_begin_use(cai_cli_pouch *pouch) {
  cai_cli_pouch_end_use(pouch);
  pouch->capture_session = 1;
}
void cai_cli_pouch_end_use(cai_cli_pouch *pouch) {
  session_guard_close(pouch->use_guard);
  pouch->use_guard = NULL;
  pouch->capture_session = 0;
}
static int session_claim(cai_cli_pouch *pouch, const char *id,
                         cai_error *error) {
  return pouch->capture_session && pouch->use_guard == NULL
             ? session_guard_open(pouch, id, 0, &pouch->use_guard, NULL, error)
             : CAI_OK;
}
int cai_cli_pouch_begin_read(cai_cli_pouch *pouch, const char *id,
                             struct cai_cli_session_guard **guard,
                             cai_error *error) {
  return session_guard_open(pouch, id, 0, guard, NULL, error);
}
void cai_cli_pouch_end_read(struct cai_cli_session_guard *guard) {
  session_guard_close(guard);
}
int cai_cli_pouch_deletable(cai_cli_pouch *pouch, const char *id, int *eligible,
                            cai_error *error) {
  cai_cli_session_guard *guard = NULL;
  cai_error local;
  int rc, busy;
  *eligible = 0;
  cai_error_init(&local);
  rc = session_guard_open(pouch, id, 1, &guard, &busy, &local);
  if (rc == CAI_OK)
    *eligible = 1;
  else if (busy)
    rc = CAI_OK;
  else
    rc = cai_cli_error(error, rc,
                       local.message != NULL ? local.message
                                             : "inspect conversation use");
  session_guard_close(guard);
  cai_error_cleanup(&local);
  return rc;
}
