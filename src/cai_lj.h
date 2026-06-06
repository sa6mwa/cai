#ifndef CAI_LJ_H
#define CAI_LJ_H

#include <lonejson.h>

lonejson *cai_lonejson_runtime(void);
lonejson *cai_lonejson_runtime_preserve(void);
lonejson *cai_lonejson_runtime_stream(void);
int cai_lonejson_runtime_open(const lonejson_config *config,
                              lonejson **out_runtime, cai_error *error);
void cai_lonejson_runtime_close(lonejson **runtime);

#define CAI_LJ cai_lonejson_runtime()
#define CAI_LJ_PRESERVE cai_lonejson_runtime_preserve()
#define CAI_LJ_STREAM cai_lonejson_runtime_stream()

#endif
