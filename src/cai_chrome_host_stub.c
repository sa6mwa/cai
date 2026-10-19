#include "cai_internal.h"
#include <cai/chrome.h>
int cai_chrome_native_host_run(const char *origin, struct pslog_logger *logger,
                               cai_error *error) {
  (void)origin;
  (void)logger;
  return cai_set_error(error, CAI_ERR_INVALID,
                       "Chrome native host requires Linux");
}
int cai_chrome_native_host_setup(const char *manifest, int remove,
                                 struct pslog_logger *logger,
                                 cai_error *error) {
  (void)manifest;
  (void)remove;
  (void)logger;
  return cai_set_error(error, CAI_ERR_INVALID,
                       "Chrome native host setup requires Linux");
}
