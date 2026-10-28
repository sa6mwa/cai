#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "../cli/temp.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int exists(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0;
}
int main(int argc, char **argv) {
  static const char *const answers[] = {"yes\n", "Y\n", "n\n",
                                        "\n",    "",    "not yes\n"};
  cai_cli_options options;
  cai_cli_temp temp, nested;
  cai_error error;
  char original[4096], current[4096], outside[4096], link[4096];
  struct stat st;
  int kept, pipefd[2], saved_input;
  size_t i;
  FILE *file;
  assert(argc == 2 && getcwd(original, sizeof(original)) != NULL);
  snprintf(outside, sizeof(outside), "%s/sentinel", argv[1]);
  file = fopen(outside, "w");
  assert(file != NULL && fputs("outside", file) >= 0 && fclose(file) == 0);
  cai_error_init(&error);
  for (i = 0U; i < sizeof(answers) / sizeof(answers[0]); i++) {
    cai_cli_options_init(&options);
    options.cache_dir = argv[1];
    cai_cli_temp_init(&temp);
    assert(cai_cli_temp_open(&temp, &options, &error) == CAI_OK);
    assert(stat(temp.root, &st) == 0 && (st.st_mode & 0777U) == 0700U);
    assert(options.lockd == NULL);
    assert(getcwd(current, sizeof(current)) != NULL &&
           strcmp(current, temp.workspace) == 0);
    assert(symlink(outside, "outside-link") == 0);
    snprintf(link, sizeof(link), "%s/outside-link", temp.private_dir);
    assert(symlink(outside, link) == 0);
    assert(pipe(pipefd) == 0);
    assert(write(pipefd[1], answers[i], strlen(answers[i])) ==
           (ssize_t)strlen(answers[i]));
    close(pipefd[1]);
    saved_input = dup(STDIN_FILENO);
    assert(saved_input >= 0 && dup2(pipefd[0], STDIN_FILENO) >= 0);
    close(pipefd[0]);
    assert(cai_cli_temp_close(&temp, 0, 1, -1, NULL, NULL, &kept, &error) ==
           CAI_OK);
    assert(dup2(saved_input, STDIN_FILENO) >= 0);
    close(saved_input);
    assert(kept == (i >= 2U));
    assert(!exists(temp.private_dir) && exists(outside));
    assert(exists(temp.workspace) == kept && exists(temp.root) == kept);
    assert(getcwd(current, sizeof(current)) != NULL &&
           strcmp(current, original) == 0);
  }
  cai_cli_options_init(&options);
  options.cache_dir = argv[1];
  cai_cli_temp_init(&temp);
  assert(cai_cli_temp_open(&temp, &options, &error) == CAI_OK);
  options.cache_dir = argv[1];
  cai_cli_temp_init(&nested);
  assert(cai_cli_temp_open(&nested, &options, &error) == CAI_OK);
  assert(cai_cli_temp_close(&nested, 1, 0, -1, NULL, NULL, &kept, &error) ==
             CAI_OK &&
         !kept);
  assert(getcwd(current, sizeof(current)) != NULL &&
         strcmp(current, temp.workspace) == 0);
  assert(cai_cli_temp_close(&temp, 1, 0, -1, NULL, NULL, &kept, &error) ==
             CAI_OK &&
         !kept);
  assert(getcwd(current, sizeof(current)) != NULL &&
         strcmp(current, original) == 0);
  options.cache_dir = outside;
  cai_cli_temp_init(&temp);
  assert(cai_cli_temp_open(&temp, &options, &error) != CAI_OK);
  cai_error_cleanup(&error);
  assert(cai_cli_temp_close(&temp, 0, 0, -1, NULL, NULL, &kept, &error) ==
         CAI_OK);
  cai_error_cleanup(&error);
  return 0;
}
