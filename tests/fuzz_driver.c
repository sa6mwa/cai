#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

static int cai_fuzz_driver_run_stream(FILE *input) {
  unsigned char *data;
  size_t capacity;
  size_t size;
  size_t count;
  int status;

  capacity = 4096U;
  size = 0U;
  status = 0;
  data = (unsigned char *)malloc(capacity);
  if (data == NULL) {
    return 1;
  }
  while ((count = fread(data + size, 1U, capacity - size, input)) != 0U) {
    size += count;
    if (size == capacity) {
      unsigned char *grown;

      if (capacity > (size_t)-1 / 2U) {
        status = 1;
        break;
      }
      capacity *= 2U;
      grown = (unsigned char *)realloc(data, capacity);
      if (grown == NULL) {
        status = 1;
        break;
      }
      data = grown;
    }
  }
  if (ferror(input)) {
    status = 1;
  }
  if (status == 0) {
    (void)LLVMFuzzerTestOneInput(data, size);
  }
  free(data);
  return status;
}

static int cai_fuzz_driver_run_file(const char *path) {
  FILE *input;
  int status;

  input = fopen(path, "rb");
  if (input == NULL) {
    fprintf(stderr, "fuzz driver: unable to read %s\n", path);
    return 1;
  }
  status = cai_fuzz_driver_run_stream(input);
  fclose(input);
  return status;
}

static int cai_fuzz_driver_run_path(const char *path) {
  DIR *directory;
  struct dirent *entry;
  struct stat metadata;
  char child[4096];
  int status;
  int written;

  if (stat(path, &metadata) != 0) {
    fprintf(stderr, "fuzz driver: unable to stat %s\n", path);
    return 1;
  }
  if (!S_ISDIR(metadata.st_mode)) {
    return cai_fuzz_driver_run_file(path);
  }
  directory = opendir(path);
  if (directory == NULL) {
    fprintf(stderr, "fuzz driver: unable to open corpus %s\n", path);
    return 1;
  }
  status = 0;
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(child)) {
      status = 1;
      break;
    }
    if (stat(child, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
        cai_fuzz_driver_run_file(child) != 0) {
      status = 1;
      break;
    }
  }
  closedir(directory);
  return status;
}

int main(int argc, char **argv) {
  int index;
  int status;

  if (argc == 1) {
    return cai_fuzz_driver_run_stream(stdin);
  }
  status = 0;
  for (index = 1; index < argc; ++index) {
    if (cai_fuzz_driver_run_path(argv[index]) != 0) {
      status = 1;
    }
  }
  return status;
}
