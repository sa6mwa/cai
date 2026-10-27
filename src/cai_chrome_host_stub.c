#include "cai_chrome_host.h"
#include "cai_internal.h"
#include <cai/chrome.h>
int cai_chrome_native_host_setup_at(const char *program_name,
                                    const char *manifest, int remove,
                                    const char *directory,
                                    struct pslog_logger *logger,
                                    cai_error *error) {
  (void)directory;
  return cai_chrome_native_host_setup_named(program_name, manifest, remove,
                                            logger, error);
}
int cai_chrome_native_host_setup_named(const char *program_name,
                                       const char *manifest, int remove,
                                       struct pslog_logger *logger,
                                       cai_error *error) {
  (void)program_name;
  return cai_chrome_native_host_setup(manifest, remove, logger, error);
}
int cai_chrome_native_host_run_at(const char *origin, const char *directory,
                                  struct pslog_logger *logger,
                                  cai_error *error) {
  (void)directory;
  return cai_chrome_native_host_run(origin, logger, error);
}
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
