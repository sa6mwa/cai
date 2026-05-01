#include <cai/cai.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAI_INTEGRATION_E2E_DEFAULT_SPEND_LIMIT_USD 0.02

static void print_error(const char *operation, int rc, const cai_error *error) {
  fprintf(stderr, "%s failed: %s\n", operation,
          error->message != NULL ? error->message : cai_status_string(rc));
  if (error->detail != NULL) {
    fprintf(stderr, "detail: %s\n", error->detail);
  }
}

static const char *integration_model(void) {
  const char *model;

  model = getenv("CAI_TEST_MODEL");
  if (model == NULL || model[0] == '\0') {
    model = CAI_MODEL_GPT_5_NANO;
  }
  return model;
}

static const char *openrouter_integration_model(void) {
  const char *model;

  model = getenv("CAI_OPENROUTER_TEST_MODEL");
  if (model == NULL || model[0] == '\0') {
    model = CAI_OPENROUTER_MODEL_DEFAULT_RESPONSES;
  }
  return model;
}

static int run_basic_response(void) {
  const char *model;
  cai_client_config client_config;
  cai_response_create_params *params;
  cai_response *response;
  cai_client *client;
  cai_error error;
  int rc;

  model = integration_model();
  cai_error_init(&error);
  cai_client_config_init(&client_config);
  client = NULL;
  params = NULL;
  response = NULL;

  rc = cai_client_open(&client_config, &client, &error);
  if (rc != CAI_OK) {
    print_error("client open", rc, &error);
    goto done;
  }
  rc = cai_response_create_params_new(&params, &error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_model(params, model, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_add_text(
        params, "user", "Reply with exactly: pong", &error);
  }
  if (rc == CAI_OK) {
    rc = cai_client_create_response(client, params, &response, &error);
  }
  if (rc != CAI_OK) {
    print_error("integration response", rc, &error);
    goto done;
  }
  if (cai_response_output_text(response) == NULL ||
      cai_response_output_text(response)[0] == '\0') {
    fprintf(stderr, "integration response had no output text\n");
    rc = CAI_ERR_PROTOCOL;
    goto done;
  }

done:
  cai_response_destroy(response);
  cai_response_create_params_destroy(params);
  cai_client_close(client);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}

static int run_openrouter_basic_response(void) {
  const char *model;
  cai_client_config client_config;
  cai_response_create_params *params;
  cai_response *response;
  cai_client *client;
  cai_error error;
  const char *answer;
  int rc;

  model = openrouter_integration_model();
  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_client_config_use_openrouter(&client_config);
  client = NULL;
  params = NULL;
  response = NULL;
  answer = NULL;

  rc = cai_client_open(&client_config, &client, &error);
  if (rc != CAI_OK) {
    print_error("openrouter client open", rc, &error);
    goto done;
  }
  rc = cai_response_create_params_new(&params, &error);
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_model(params, model, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_instructions(
        params,
        "You are a strict API compatibility test. Reply with exactly the "
        "requested marker and no other text.",
        &error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_max_output_tokens(params, 32, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_set_reasoning(
        params, CAI_REASONING_EFFORT_NONE, NULL, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_response_create_params_add_text(
        params, "user", "Reply with exactly: openrouter-pong-314", &error);
  }
  if (rc == CAI_OK) {
    rc = cai_client_create_response(client, params, &response, &error);
  }
  if (rc != CAI_OK) {
    print_error("openrouter integration response", rc, &error);
    goto done;
  }
  answer = cai_response_output_text(response);
  if (answer == NULL || strstr(answer, "openrouter-pong-314") == NULL) {
    fprintf(stderr, "openrouter response failed marker check:\n%s\n",
            answer != NULL ? answer : "(null)");
    rc = CAI_ERR_PROTOCOL;
    goto done;
  }

done:
  cai_response_destroy(response);
  cai_response_create_params_destroy(params);
  cai_client_close(client);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}

static int run_openrouter_session_regression(void) {
  static const char secret[] = "openrouter-session-key-271";
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_response *response;
  cai_error error;
  const char *answer;
  int rc;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_client_config_use_openrouter(&client_config);
  cai_agent_config_init(&agent_config);
  client = NULL;
  agent = NULL;
  session = NULL;
  response = NULL;
  answer = NULL;

  agent_config.model = openrouter_integration_model();
  agent_config.developer_instructions =
      "You are a strict OpenRouter session regression assistant. Remember "
      "exact keys. When asked to recall a key, answer with only the key.";
  agent_config.reasoning_effort = CAI_REASONING_EFFORT_NONE;
  agent_config.max_output_tokens = 64;
  agent_config.session_continuity = CAI_SESSION_CONTINUITY_CLIENT_HISTORY;

  rc = cai_client_open(&client_config, &client, &error);
  if (rc == CAI_OK) {
    rc = cai_client_new_agent(client, &agent_config, &agent, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_agent_new_session(agent, &session, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_send_text(
        session,
        "Remember this exact OpenRouter session key: "
        "openrouter-session-key-271. Reply with only ok.",
        &response, &error);
  }
  cai_response_destroy(response);
  response = NULL;
  if (rc == CAI_OK) {
    rc = cai_session_send_text(
        session,
        "Recall the exact OpenRouter session key I asked you to remember.",
        &response, &error);
  }
  if (rc != CAI_OK) {
    print_error("openrouter session regression", rc, &error);
    goto done;
  }
  answer = cai_response_output_text(response);
  if (answer == NULL || strstr(answer, secret) == NULL) {
    fprintf(stderr, "openrouter session answer did not preserve key:\n%s\n",
            answer != NULL ? answer : "(null)");
    rc = CAI_ERR_PROTOCOL;
    goto done;
  }

done:
  cai_response_destroy(response);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}

static int send_and_destroy(cai_session *session, const char *text,
                            cai_error *error) {
  cai_response *response;
  int rc;

  response = NULL;
  rc = cai_session_send_text(session, text, &response, error);
  cai_response_destroy(response);
  return rc;
}

static double integration_spend_limit_usd(void) {
  const char *value;
  double parsed;

  value = getenv("CAI_INTEGRATION_SPEND_LIMIT_USD");
  if (value == NULL || value[0] == '\0') {
    return CAI_INTEGRATION_E2E_DEFAULT_SPEND_LIMIT_USD;
  }
  parsed = atof(value);
  return parsed > 0.0 ? parsed : CAI_INTEGRATION_E2E_DEFAULT_SPEND_LIMIT_USD;
}

static double usage_estimate_usd(const char *model,
                                 const cai_token_usage *usage) {
  if (usage == NULL) {
    return 0.0;
  }
  return cai_model_estimate_usage_usd(model, usage->input_tokens,
                                      usage->input_cached_tokens,
                                      usage->output_tokens);
}

static int answer_contains(const char *answer, const char *needle) {
  return answer != NULL && needle != NULL && strstr(answer, needle) != NULL;
}

static int answer_contains_turn(const char *answer, int turn) {
  char plain[32];
  char bracketed[32];

  snprintf(plain, sizeof(plain), "TURN=%d", turn);
  snprintf(bracketed, sizeof(bracketed), "TURN=<%d>", turn);
  return answer_contains(answer, plain) || answer_contains(answer, bracketed);
}

static int answer_contains_previous_secret(const char *answer,
                                           const char *previous_secret) {
  if (strcmp(previous_secret, "none") == 0) {
    return answer_contains(answer, "none") || answer_contains(answer, "None") ||
           answer_contains(answer, "NONE");
  }
  return answer_contains(answer, previous_secret);
}

static int run_e2e_session_regression(void) {
  static const char first_secret[] = "alpha-173";
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_response *response;
  cai_token_usage usage;
  cai_error error;
  const char *model;
  const char *answer;
  char prompt[512];
  char current_secret[64];
  char previous_secret[64];
  char expected_turn[64];
  char expected_first[64];
  char expected_previous[64];
  char expected_current[64];
  double spent_usd;
  double limit_usd;
  int rc;
  int turn;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  client = NULL;
  agent = NULL;
  session = NULL;
  response = NULL;
  model = integration_model();
  spent_usd = 0.0;
  limit_usd = integration_spend_limit_usd();

  agent_config.model = model;
  agent_config.developer_instructions =
      "You are a strict regression-test assistant. Remember the first secret "
      "and every turn secret. For every user turn, answer with one line using "
      "actual values only: TURN=number FIRST=value PREV=value CURRENT=value. "
      "Never print angle brackets, placeholder names, or explanatory text.";
  agent_config.reasoning_effort = CAI_REASONING_EFFORT_MINIMAL;
  agent_config.max_output_tokens = 80;

  rc = cai_client_open(&client_config, &client, &error);
  if (rc == CAI_OK) {
    rc = cai_client_new_agent(client, &agent_config, &agent, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_agent_new_session(agent, &session, &error);
  }
  for (turn = 1; rc == CAI_OK && turn <= 20; turn++) {
    snprintf(current_secret, sizeof(current_secret), "turn-%02d-key-%03d", turn,
             700 + turn);
    if (turn == 1) {
      strcpy(previous_secret, "none");
      snprintf(prompt, sizeof(prompt),
               "This is turn 1. Store first_secret=%s and current_secret=%s. "
               "There is no previous turn, so PREV is none. Report the actual "
               "stored values now.",
               first_secret, current_secret);
    } else {
      snprintf(previous_secret, sizeof(previous_secret), "turn-%02d-key-%03d",
               turn - 1, 700 + turn - 1);
      snprintf(prompt, sizeof(prompt),
               "This is turn %d. Store current_secret=%s. Report using the "
               "first_secret from turn 1 and the previous turn secret from "
               "memory. Use actual values only.",
               turn, current_secret);
    }
    response = NULL;
    rc = cai_session_send_text(session, prompt, &response, &error);
    if (rc != CAI_OK) {
      break;
    }
    answer = cai_response_output_text(response);
    snprintf(expected_turn, sizeof(expected_turn), "TURN=%d", turn);
    snprintf(expected_first, sizeof(expected_first), "FIRST=%s", first_secret);
    snprintf(expected_previous, sizeof(expected_previous), "PREV=%s",
             previous_secret);
    snprintf(expected_current, sizeof(expected_current), "CURRENT=%s",
             current_secret);
    if (!answer_contains_turn(answer, turn) ||
        !answer_contains(answer, first_secret) ||
        !answer_contains_previous_secret(answer, previous_secret) ||
        !answer_contains(answer, current_secret)) {
      fprintf(stderr,
              "integration e2e turn %d failed content check\nexpected: %s %s %s %s\n"
              "answer: %s\n",
              turn, expected_turn, expected_first, expected_previous,
              expected_current, answer != NULL ? answer : "(null)");
      rc = CAI_ERR_PROTOCOL;
      break;
    }
    memset(&usage, 0, sizeof(usage));
    if (cai_session_last_usage(session, &usage, &error) == CAI_OK) {
      spent_usd += usage_estimate_usd(model, &usage);
      fprintf(stderr,
              "[integration-e2e] turn=%d tokens=%lld cached=%lld estimated_cost=$%.8f"
              " limit=$%.8f\n",
              turn, usage.total_tokens, usage.input_cached_tokens, spent_usd,
              limit_usd);
      if (spent_usd > limit_usd) {
        fprintf(stderr,
                "integration e2e estimated spend exceeded limit: %.8f > %.8f\n",
                spent_usd, limit_usd);
        rc = CAI_ERR_INVALID;
        break;
      }
    } else {
      print_error("integration e2e usage", error.code, &error);
      rc = error.code != CAI_OK ? error.code : CAI_ERR_PROTOCOL;
      break;
    }
    cai_response_destroy(response);
    response = NULL;
  }

  if (rc != CAI_OK) {
    print_error("integration e2e session regression", rc, &error);
  }
  cai_response_destroy(response);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}

static int run_compaction_recall(void) {
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_response *response;
  cai_error error;
  const char *answer;
  int rc;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  client = NULL;
  agent = NULL;
  session = NULL;
  response = NULL;

  agent_config.model = integration_model();
  agent_config.developer_instructions =
      "You are a deterministic recall test assistant. Store compact test "
      "facts exactly. When asked to recall them, answer with only the stored "
      "facts.";
  agent_config.reasoning_effort = CAI_REASONING_EFFORT_MINIMAL;
  agent_config.max_output_tokens = 512;
  agent_config.compact_threshold_tokens = 1000LL;

  rc = cai_client_open(&client_config, &client, &error);
  if (rc == CAI_OK) {
    rc = cai_client_new_agent(client, &agent_config, &agent, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_agent_new_session(agent, &session, &error);
  }
  if (rc == CAI_OK) {
    rc = send_and_destroy(session,
                          "Compact test fact one: project codename is Blue "
                          "Quartz.",
                          &error);
  }
  if (rc == CAI_OK) {
    rc = send_and_destroy(session,
                          "Compact test fact two: launch number is 17.",
                          &error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_send_text(
        session,
        "Recall the compact test facts. The answer must include the codename "
        "and launch number.",
        &response, &error);
  }
  if (rc != CAI_OK) {
    print_error("integration compaction recall", rc, &error);
    goto done;
  }
  answer = cai_response_output_text(response);
  if (answer == NULL || strstr(answer, "Blue Quartz") == NULL ||
      strstr(answer, "17") == NULL) {
    fprintf(stderr, "compaction recall answer did not preserve facts:\n%s\n",
            answer != NULL ? answer : "(null)");
    rc = CAI_ERR_PROTOCOL;
    goto done;
  }

done:
  cai_response_destroy(response);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}

static int run_state_restore_regression(void) {
  static const char saved_secret[] = "state-restore-key-418";
  cai_agent_config agent_config;
  cai_client_config client_config;
  cai_client *client;
  cai_agent *agent;
  cai_session *session;
  cai_session *restored;
  cai_response *response;
  cai_token_usage usage;
  cai_error error;
  const char *model;
  const char *answer;
  char state_path[] = "/tmp/cai-integration-state-XXXXXX";
  double spent_usd;
  double limit_usd;
  int state_fd;
  int rc;

  cai_error_init(&error);
  cai_client_config_init(&client_config);
  cai_agent_config_init(&agent_config);
  client = NULL;
  agent = NULL;
  session = NULL;
  restored = NULL;
  response = NULL;
  model = integration_model();
  spent_usd = 0.0;
  limit_usd = integration_spend_limit_usd();
  state_fd = mkstemp(state_path);
  if (state_fd < 0) {
    fprintf(stderr, "failed to allocate state restore temp path\n");
    cai_error_cleanup(&error);
    return 1;
  }
  close(state_fd);
  unlink(state_path);

  agent_config.model = model;
  agent_config.developer_instructions =
      "You are a strict state-restore regression assistant. Remember exact "
      "keys. When asked to recall a key, answer with only the key.";
  agent_config.reasoning_effort = CAI_REASONING_EFFORT_MINIMAL;
  agent_config.max_output_tokens = 64;
  agent_config.enable_local_history = 1;

  rc = cai_client_open(&client_config, &client, &error);
  if (rc == CAI_OK) {
    rc = cai_client_new_agent(client, &agent_config, &agent, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_agent_new_session(agent, &session, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_send_text(
        session,
        "Remember the exact state restore key: state-restore-key-418. "
        "Reply with only ok.",
        &response, &error);
  }
  if (rc == CAI_OK) {
    memset(&usage, 0, sizeof(usage));
    if (cai_session_last_usage(session, &usage, &error) == CAI_OK) {
      spent_usd += usage_estimate_usd(model, &usage);
    }
  }
  cai_response_destroy(response);
  response = NULL;
  if (rc == CAI_OK) {
    rc = cai_session_save_state_path(session, state_path, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_agent_new_session(agent, &restored, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_load_state_path(restored, state_path, &error);
  }
  if (rc == CAI_OK) {
    rc = cai_session_send_text(
        restored,
        "Recall the exact state restore key I asked you to remember.",
        &response, &error);
  }
  if (rc != CAI_OK) {
    print_error("integration state restore", rc, &error);
    goto done;
  }
  answer = cai_response_output_text(response);
  if (answer == NULL || strstr(answer, saved_secret) == NULL) {
    fprintf(stderr, "state restore answer did not preserve key:\n%s\n",
            answer != NULL ? answer : "(null)");
    rc = CAI_ERR_PROTOCOL;
    goto done;
  }
  memset(&usage, 0, sizeof(usage));
  if (cai_session_last_usage(restored, &usage, &error) == CAI_OK) {
    spent_usd += usage_estimate_usd(model, &usage);
    fprintf(stderr,
            "[integration-state-restore] tokens=%lld cached=%lld "
            "estimated_cost=$%.8f limit=$%.8f\n",
            usage.total_tokens, usage.input_cached_tokens, spent_usd,
            limit_usd);
    if (spent_usd > limit_usd) {
      fprintf(stderr,
              "integration state restore estimated spend exceeded limit: "
              "%.8f > %.8f\n",
              spent_usd, limit_usd);
      rc = CAI_ERR_INVALID;
    }
  } else {
    print_error("integration state restore usage", error.code, &error);
    rc = error.code != CAI_OK ? error.code : CAI_ERR_PROTOCOL;
  }

done:
  unlink(state_path);
  cai_response_destroy(response);
  cai_session_destroy(restored);
  cai_session_destroy(session);
  cai_agent_destroy(agent);
  cai_client_close(client);
  cai_error_cleanup(&error);
  return rc == CAI_OK ? 0 : 1;
}

int main(void) {
  const char *compaction;
  const char *e2e;
  const char *openrouter;
  const char *openrouter_session;
  const char *state_restore;

  openrouter_session = getenv("CAI_INTEGRATION_OPENROUTER_SESSION");
  if (openrouter_session != NULL && openrouter_session[0] != '\0' &&
      strcmp(openrouter_session, "0") != 0) {
    return run_openrouter_session_regression();
  }
  openrouter = getenv("CAI_INTEGRATION_OPENROUTER");
  if (openrouter != NULL && openrouter[0] != '\0' &&
      strcmp(openrouter, "0") != 0) {
    return run_openrouter_basic_response();
  }
  e2e = getenv("CAI_INTEGRATION_E2E");
  if (e2e != NULL && e2e[0] != '\0' && strcmp(e2e, "0") != 0) {
    return run_e2e_session_regression();
  }
  state_restore = getenv("CAI_INTEGRATION_STATE_RESTORE");
  if (state_restore != NULL && state_restore[0] != '\0' &&
      strcmp(state_restore, "0") != 0) {
    return run_state_restore_regression();
  }
  compaction = getenv("CAI_INTEGRATION_COMPACTION");
  if (compaction != NULL && compaction[0] != '\0' &&
      strcmp(compaction, "0") != 0) {
    return run_compaction_recall();
  }
  return run_basic_response();
}
