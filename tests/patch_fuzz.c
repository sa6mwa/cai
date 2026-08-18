#include "cai_internal.h"

#include <cai/tools/patch.h>

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CAI_PATCH_FUZZ_MAX_INPUT (128U * 1024U)

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static int cai_patch_fuzz_sink_write(void *context, const void *bytes,
                                     size_t count, cai_error *error) {
  (void)context;
  (void)bytes;
  (void)count;
  (void)error;
  return CAI_OK;
}

static int cai_patch_fuzz_write_file(const char *path, const char *text) {
  FILE *file;
  size_t length;

  file = fopen(path, "wb");
  if (file == NULL) {
    return 0;
  }
  length = strlen(text);
  if (length != 0U && fwrite(text, 1U, length, file) != length) {
    fclose(file);
    return 0;
  }
  return fclose(file) == 0;
}

static void cai_patch_fuzz_remove_tree(const char *path) {
  DIR *directory;
  struct dirent *entry;

  directory = opendir(path);
  if (directory == NULL) {
    (void)unlink(path);
    return;
  }
  while ((entry = readdir(directory)) != NULL) {
    char child[PATH_MAX];
    struct stat metadata;
    int written;

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(child) ||
        lstat(child, &metadata) != 0) {
      continue;
    }
    if (S_ISDIR(metadata.st_mode)) {
      cai_patch_fuzz_remove_tree(child);
    } else {
      (void)unlink(child);
    }
  }
  closedir(directory);
  (void)rmdir(path);
}

static int cai_patch_fuzz_new_root(char *root, size_t root_size) {
  char nested[PATH_MAX];
  char alpha[PATH_MAX];
  char omega[PATH_MAX];
  int written;

  written = snprintf(root, root_size, "/tmp/cai-patch-fuzz-XXXXXX");
  if (written < 0 || (size_t)written >= root_size || mkdtemp(root) == NULL) {
    return 0;
  }
  written = snprintf(nested, sizeof(nested), "%s/nested", root);
  if (written < 0 || (size_t)written >= sizeof(nested) ||
      mkdir(nested, 0700) != 0) {
    cai_patch_fuzz_remove_tree(root);
    return 0;
  }
  written = snprintf(alpha, sizeof(alpha), "%s/alpha.txt", root);
  if (written < 0 || (size_t)written >= sizeof(alpha) ||
      !cai_patch_fuzz_write_file(alpha, "one\ntwo\nthree\n")) {
    cai_patch_fuzz_remove_tree(root);
    return 0;
  }
  written = snprintf(omega, sizeof(omega), "%s/nested/omega.txt", root);
  if (written < 0 || (size_t)written >= sizeof(omega) ||
      !cai_patch_fuzz_write_file(omega, "omega\n")) {
    cai_patch_fuzz_remove_tree(root);
    return 0;
  }
  return 1;
}

static size_t cai_patch_fuzz_spool_size(const lonejson_spooled *spool) {
  return spool->size;
}

static lonejson_status cai_patch_fuzz_spool_rewind(lonejson_spooled *spool,
                                                   lonejson_error *error) {
  (void)error;
  spool->read_offset = 0U;
  return LONEJSON_STATUS_OK;
}

static lonejson_read_result cai_patch_fuzz_spool_read(lonejson_spooled *spool,
                                                      unsigned char *buffer,
                                                      size_t capacity) {
  lonejson_read_result result;
  size_t chunk;
  size_t remaining;

  result = lonejson_default_read_result();
  if (capacity == 0U || spool->read_offset >= spool->size) {
    result.eof = 1;
    return result;
  }
  remaining = spool->size - spool->read_offset;
  chunk = 1U + (spool->memory[spool->read_offset] % 31U);
  if (chunk > capacity) {
    chunk = capacity;
  }
  if (chunk > remaining) {
    chunk = remaining;
  }
  memcpy(buffer, spool->memory + spool->read_offset, chunk);
  spool->read_offset += chunk;
  result.bytes_read = chunk;
  result.eof = spool->read_offset == spool->size;
  return result;
}

static void cai_patch_fuzz_run(const unsigned char *data, size_t size,
                               int use_spooled_path) {
  cai_patch_tool_config config;
  cai_tool_registry *registry;
  cai_sink_callbacks callbacks;
  cai_sink *sink;
  cai_error error;
  lonejson_spooled spool;
  char root[PATH_MAX];
  char *patch;
  unsigned char *spool_data;

  if (!cai_patch_fuzz_new_root(root, sizeof(root))) {
    return;
  }
  memset(&config, 0, sizeof(config));
  config.root_path = root;
  config.max_patch_bytes = CAI_PATCH_FUZZ_MAX_INPUT;
  config.max_file_bytes = CAI_PATCH_FUZZ_MAX_INPUT;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.write = cai_patch_fuzz_sink_write;
  cai_error_init(&error);
  sink = NULL;
  registry = NULL;
  patch = NULL;
  spool_data = NULL;
  memset(&spool, 0, sizeof(spool));

  if (cai_sink_from_callbacks(&callbacks, &sink, &error) == CAI_OK) {
    if (use_spooled_path) {
      if (size > 0U) {
        spool_data = (unsigned char *)malloc(size);
        if (spool_data != NULL) {
          memcpy(spool_data, data, size);
        }
      }
      if (size == 0U || spool_data != NULL) {
        spool.memory = spool_data;
        spool.memory_len = size;
        spool.size = size;
        spool.size_fn = cai_patch_fuzz_spool_size;
        spool.rewind = cai_patch_fuzz_spool_rewind;
        spool.read = cai_patch_fuzz_spool_read;
        if (cai_tool_registry_new(&registry, &error) == CAI_OK &&
            cai_tool_registry_register_patch_tool(registry, &config, &error) ==
                CAI_OK) {
          (void)cai_tool_registry_run_spooled(
              registry, CAI_PATCH_DEFAULT_TOOL_NAME, &spool, sink, &error);
        }
      }
    } else {
      patch = (char *)malloc(size + 1U);
      if (patch != NULL) {
        if (size != 0U) {
          memcpy(patch, data, size);
        }
        patch[size] = '\0';
        (void)cai_apply_patch(&config, patch, sink, &error);
      }
    }
  }
  free(patch);
  free(spool_data);
  cai_tool_registry_destroy(registry);
  cai_sink_close(sink);
  cai_error_cleanup(&error);
  cai_patch_fuzz_remove_tree(root);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  if (size > CAI_PATCH_FUZZ_MAX_INPUT) {
    return 0;
  }
  cai_patch_fuzz_run(data, size, 0);
  cai_patch_fuzz_run(data, size, 1);
  return 0;
}
