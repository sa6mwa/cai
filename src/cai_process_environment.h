#ifndef CAI_PROCESS_ENVIRONMENT_H
#define CAI_PROCESS_ENVIRONMENT_H
#include <cai/cai.h>
/* Snapshot before fork. Restricted children preserve HOME, PATH and every
 * XDG_* entry verbatim; YOLO children inherit the complete environment. */
int cai_process_environment_new(int yolo, const char *const *defaults,
                                char ***out, cai_error *error);
void cai_process_environment_free(char **environment);
#endif
