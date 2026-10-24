/* Exercise the packaged storage SDK through CAI's actual encrypted backend.
 * Only public liblockdc APIs are used; no upstream internal headers/hooks. */
#include "../cli/pouch.c"
#include <assert.h>

static const char prefix[] =
    "{\"id\":\"compaction-fixture\",\"workspace\":\"/fixture\","
    "\"published\":0,\"padding\":\"";
static const char suffix[] = "\"}";
#define PADDING_BYTES (8U * 1024U * 1024U)
#define DOCUMENT_BYTES                                                         \
  (sizeof(prefix) - 1U + PADDING_BYTES + sizeof(suffix) - 1U)

/* The SDK's public settings do not expose its 30-minute compaction interval.
 * Shorten that deadline only in this test executable via the linker, retaining
 * the packaged worker, its default pthread stack and real snapshot operations.
 * Other timed waits retain their original deadlines. */
int __real_pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *,
                                  const struct timespec *);
int __wrap_pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *,
                                  const struct timespec *);
int __wrap_pthread_cond_timedwait(pthread_cond_t *condition,
                                  pthread_mutex_t *mutex,
                                  const struct timespec *deadline) {
  struct timespec now, shortened;
  /* musl also uses this symbol internally for untimed waits with NULL. */
  if (deadline == NULL)
    return __real_pthread_cond_timedwait(condition, mutex, deadline);
  assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
  /* Repeated mutation wakeups reuse the original absolute deadline. Recognize
   * it for this fixture's entire 60-second budget, and translate that deadline
   * rather than resetting the shortened wait on every wakeup. */
  if (deadline->tv_sec - now.tv_sec >= 1740 &&
      deadline->tv_sec - now.tv_sec <= 1800) {
    shortened = *deadline;
    shortened.tv_sec -= 1799;
    deadline = &shortened;
  }
  return __real_pthread_cond_timedwait(condition, mutex, deadline);
}

static size_t document_read(void *context, void *bytes, size_t count,
                            cai_error *error) {
  size_t *offset = context;
  size_t i;
  unsigned char *out = bytes;
  (void)error;
  if (count > DOCUMENT_BYTES - *offset)
    count = DOCUMENT_BYTES - *offset;
  for (i = 0U; i < count; i++, (*offset)++) {
    if (*offset < sizeof(prefix) - 1U)
      out[i] = (unsigned char)prefix[*offset];
    else if (*offset < sizeof(prefix) - 1U + PADDING_BYTES)
      out[i] = (unsigned char)('a' + *offset % 26U);
    else
      out[i] =
          (unsigned char)suffix[*offset - sizeof(prefix) + 1U - PADDING_BYTES];
  }
  return count;
}

static cai_source *document_source(size_t *offset, cai_error *error) {
  cai_source_callbacks callbacks;
  cai_source *source = NULL;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.read = document_read;
  callbacks.context = offset;
  *offset = 0U;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  return source;
}

static void verify_document(cai_source *source, cai_error *error) {
  unsigned char actual[4096], expected[4096];
  size_t offset = 0U;
  size_t count;
  assert(source != NULL);
  while ((count = cai_source_read(source, actual, sizeof(actual), error)) !=
         0U) {
    assert(count == document_read(&offset, expected, count, error));
    assert(memcmp(actual, expected, count) == 0);
  }
  assert(error->code == CAI_OK && offset == DOCUMENT_BYTES);
  cai_source_close(source);
}

static void write_document(cai_cli_pouch *pouch, const char *key,
                           cai_error *error) {
  cai_source *source;
  lc_lease *lease = NULL;
  size_t offset;
  assert(acquire(pouch, key, &lease, error) == CAI_OK);
  source = document_source(&offset, error);
  assert(update(lease, source, error) == CAI_OK);
  cai_source_close(source);
  assert(release(lease, CAI_OK, error) == CAI_OK);
}

/* A published manifest proves snapshot completion, rather than just worker
 * liveness or a temporary snapshot file. */
static void wait_snapshot(cai_cli_pouch *pouch) {
  char path[PATH_MAX], line[512];
  struct timespec start, now, delay;
  FILE *file;
  int published;
  assert(snprintf(path, sizeof(path),
                  "%s/pouch/namespaces/cai.sessions/manifest",
                  pouch->state_directory) < (int)sizeof(path));
  assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
  delay.tv_sec = 0;
  delay.tv_nsec = 10000000L;
  for (;;) {
    file = fopen(path, "r");
    assert(file != NULL);
    published = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
      if (strncmp(line, "snapshot=", 9U) == 0 &&
          strncmp(line + 9, "snapshot-", 9U) == 0)
        published = 1;
    }
    assert(fclose(file) == 0);
    if (published)
      return;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    assert(now.tv_sec - start.tv_sec < 20);
    (void)nanosleep(&delay, NULL);
  }
}

int main(int argc, char **argv) {
  cai_cli_pouch pouch;
  cai_error error;
  cai_source *source;
  char endpoint[PATH_MAX + 160];
  size_t offset;
  int i;
  assert(argc == 2 && argv[1][0] == '/');
  assert(setenv("XDG_STATE_HOME", argv[1], 1) == 0);
  assert(snprintf(endpoint, sizeof(endpoint),
                  "pouch://%s/cai/pouch?segment_target_bytes=262144&"
                  "disable_compaction_throttling=true",
                  argv[1]) < (int)sizeof(endpoint));
  cai_error_init(&error);
  assert(cai_cli_pouch_open(&pouch, endpoint, NULL, NULL, &error) == CAI_OK);
  source = document_source(&offset, &error);
  assert(pouch.credentials.replace(&pouch, "fixture.json", source, &error) ==
         CAI_OK);
  cai_source_close(source);
  /* Keep a large live span in the candidate segments so the snapshot copies
   * multiple chunks, alongside the real conversation's small live records. */
  write_document(&pouch, "large-live", &error);
  /* Public settings do not expose the compaction byte threshold. Cross the
   * real 64 MiB threshold with bounded streamed input and small segments.
   * The hidden fixture document leaves conversation listings unchanged. */
  for (i = 0; i < 10; i++)
    write_document(&pouch, "compaction-fixture", &error);
  wait_snapshot(&pouch);
  cai_cli_pouch_close(&pouch);
  assert(cai_cli_pouch_open(&pouch, endpoint, NULL, NULL, &error) == CAI_OK);
  source = NULL;
  assert(download_source(&pouch, "compaction-fixture", NULL, &source, &error) ==
         CAI_OK);
  verify_document(source, &error);
  source = NULL;
  assert(download_source(&pouch, "large-live", NULL, &source, &error) ==
         CAI_OK);
  verify_document(source, &error);
  source = NULL;
  assert(pouch.credentials.load(&pouch, "fixture.json", &source, &error) ==
         CAI_OK);
  verify_document(source, &error);
  cai_cli_pouch_close(&pouch);
  cai_error_cleanup(&error);
  puts("Encrypted background snapshot and reopened payloads verified");
  return 0;
}
