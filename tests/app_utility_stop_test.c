#define _POSIX_C_SOURCE 200809L
#include "../cli/pouch.h"
#include <assert.h>
#include <cai/app.h>
#include <lc/lc.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static cai_app *app;
static const char *mode;
static int requests, acknowledgements, listener = -1, connection = -1;
static int client_polls, enqueues;
static void request_stop(void) {
  cai_error error;
  unsigned long long id;
  cai_error_init(&error);
  assert(app->control(app, CAI_APP_STOP, NULL, &id, &error) == CAI_OK);
  assert(id > 0U);
  requests++;
  cai_error_cleanup(&error);
}
static void ack(void *context, unsigned long long id, int status,
                const cai_error *error) {
  (void)context;
  (void)error;
  assert(id > 0U && status == CAI_OK);
  acknowledgements++;
}
int __real_cai_cli_pouch_open_options(cai_cli_pouch *, const cai_cli_options *,
                                      pslog_logger *, cai_error *);
int __wrap_cai_cli_pouch_open_options(cai_cli_pouch *, const cai_cli_options *,
                                      pslog_logger *, cai_error *);
int __wrap_cai_cli_pouch_open_options(cai_cli_pouch *pouch,
                                      const cai_cli_options *options,
                                      pslog_logger *logger, cai_error *error) {
  int rc = __real_cai_cli_pouch_open_options(pouch, options, logger, error);
  if (rc == CAI_OK && strcmp(mode, "message_before") == 0)
    request_stop();
  return rc;
}
int __real_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *, nfds_t, int);
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout) {
  int accepting = 0;
  socklen_t length = sizeof(accepting);
  if (strncmp(mode, "login_", 6U) == 0 && count == 2U && requests == 0 &&
      getsockopt(fds[0].fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &length) ==
          0) {
    if (accepting) {
      listener = fds[0].fd;
      if (strcmp(mode, "login_accept") == 0)
        request_stop();
      else if (connection < 0) {
        struct sockaddr_storage address;
        socklen_t size = sizeof(address);
        static const char partial[] = "GET /auth/callback HTTP/1.1\r\n";
        assert(getsockname(listener, (struct sockaddr *)&address, &size) == 0);
        connection = socket(address.ss_family, SOCK_STREAM, 0);
        assert(connection >= 0);
        assert(connect(connection, (struct sockaddr *)&address, size) == 0);
        assert(write(connection, partial, sizeof(partial) - 1U) ==
               (ssize_t)(sizeof(partial) - 1U));
      }
    } else if (listener >= 0 && ++client_polls == 2) {
      /* The first read consumed incomplete headers. STOP must wake the wait
       * for their remainder while the browser connection remains open. */
      request_stop();
    }
  }
  return __real_poll(fds, count, timeout);
}
int __real_lc_enqueue(lc_client *, const lc_enqueue_req *, lc_source *,
                      lc_enqueue_res *, lc_error *);
int __wrap_lc_enqueue(lc_client *, const lc_enqueue_req *, lc_source *,
                      lc_enqueue_res *, lc_error *);
int __wrap_lc_enqueue(lc_client *client, const lc_enqueue_req *request,
                      lc_source *source, lc_enqueue_res *result,
                      lc_error *error) {
  int rc = __real_lc_enqueue(client, request, source, result, error);
  assert(strcmp(mode, "message") == 0);
  assert(strcmp(request->queue, "utility-target") == 0);
  assert(rc == LC_OK);
  enqueues++;
  request_stop();
  return rc;
}
int main(int argc, char **argv) {
  cai_app_config config;
  cai_app_status status;
  cai_error error;
  int code = 1;
  mode = getenv("CAI_TEST_UTILITY_STOP");
  assert(mode != NULL);
  cai_app_config_init(&config);
  config.control_callback = ack;
  cai_error_init(&error);
  cai_app_status_init(&status);
  assert(cai_app_open(&config, &app, &error) == CAI_OK);
  assert(app->run(app, argc - 1, (const char *const *)(argv + 1), &code,
                  &error) == CAI_OK);
  assert(code == 0 && requests == 1 && acknowledgements == 1);
  assert(app->get_status(app, &status, &error) == CAI_OK);
  assert(status.outcome == CAI_APP_STOPPED && status.error.code == CAI_OK);
  assert(!status.running && !status.runtime_ready && status.session_id == NULL);
  if (strncmp(mode, "message", 7U) == 0)
    assert(enqueues == (strcmp(mode, "message") == 0 ? 1 : 0));
  else {
    assert(listener >= 0 && enqueues == 0);
    if (connection >= 0)
      close(connection);
  }
  app->close(app);
  cai_app_status_cleanup(&status);
  cai_error_cleanup(&error);
  return 0;
}
