/* Presentation/CLI wire tests do not assert per-event crash durability.
 * Keep the real encrypted store, JSON, leases, indexing and close/reopen,
 * but remove group-commit waits from those tests. Dedicated storage tests
 * link the production CLI and retain its durable-sync settings. */
#include "../cli/pouch_stream.h"
#include <pthread.h>

static pthread_mutex_t opens_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int opens;
unsigned int cai_test_storage_open_count(void);
unsigned int cai_test_storage_open_count(void) {
  unsigned int count;
  pthread_mutex_lock(&opens_lock);
  count = opens;
  pthread_mutex_unlock(&opens_lock);
  return count;
}

int __real_lc_client_open(const lc_client_config *, lc_client **, lc_error *);
int __wrap_lc_client_open(const lc_client_config *, lc_client **, lc_error *);

int __wrap_lc_client_open(const lc_client_config *config, lc_client **out,
                          lc_error *error) {
  lc_client_config selected = *config;
  lc_pouch_settings settings;
  pthread_mutex_lock(&opens_lock);
  opens++;
  pthread_mutex_unlock(&opens_lock);
  if (config->pouch_settings != NULL) {
    settings = *config->pouch_settings;
    settings.set_mask |= LC_POUCH_SETTING_DURABLE_SYNC;
    settings.durable_sync = 0;
    selected.pouch_settings = &settings;
  }
  return __real_lc_client_open(&selected, out, error);
}
