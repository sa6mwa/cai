#define _POSIX_C_SOURCE 200809L
#include "response.h"
#include "pouch_stream.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cai_cli_response_append(cai_cli_response *response, const char *directory,
                            const char *bytes, size_t count, cai_error *error) {
  char path[4096];
  int fd;
  if (count == 0U)
    return CAI_OK;
  if (response->file == NULL) {
    if (snprintf(path, sizeof(path), "%s/.response-XXXXXX", directory) >=
        (int)sizeof(path))
      return cai_cli_error(error, CAI_ERR_INVALID,
                           "final response spool path too long");
    fd = mkstemp(path);
    if (fd < 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "create private final response spool");
    if (unlink(path) != 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
      close(fd);
      (void)unlink(path);
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "protect final response spool");
    }
    response->file = fdopen(fd, "w+b");
    if (response->file == NULL) {
      close(fd);
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "open final response spool");
    }
  }
  if (response->next_response) {
    if (fflush(response->file) != 0 ||
        ftruncate(fileno(response->file), 0) != 0 ||
        fseek(response->file, 0L, SEEK_SET) != 0)
      return cai_cli_error(error, CAI_ERR_TRANSPORT,
                           "reset final response spool");
    response->next_response = 0;
  }
  return fwrite(bytes, 1U, count, response->file) == count
             ? CAI_OK
             : cai_cli_error(error, CAI_ERR_TRANSPORT,
                             "retain final assistant response");
}

void cai_cli_response_boundary(cai_cli_response *response) {
  response->next_response = 1;
}

int cai_cli_response_source(cai_cli_response *response, cai_source **out,
                            cai_error *error) {
  *out = NULL;
  if (response->file == NULL)
    return CAI_OK;
  if (fflush(response->file) != 0 || fseek(response->file, 0L, SEEK_SET) != 0)
    return cai_cli_error(error, CAI_ERR_TRANSPORT, "read final response spool");
  return cai_source_file(response->file, 0, out, error);
}

void cai_cli_response_close(cai_cli_response *response) {
  if (response->file != NULL)
    fclose(response->file);
  response->file = NULL;
}
