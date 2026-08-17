#include "../cai_internal.h"

#include <cai/tools/goal.h>

#include <ctype.h>
#include <string.h>
#include <time.h>

typedef struct cai_goal_context {
  cai_session *session;
} cai_goal_context;

typedef struct cai_goal_get_args {
  long long ignored;
} cai_goal_get_args;

typedef struct cai_goal_create_args {
  char *objective;
  long long token_budget;
  int has_token_budget;
} cai_goal_create_args;

typedef struct cai_goal_update_args {
  char *status;
} cai_goal_update_args;

typedef struct cai_goal_result {
  int has_goal;
  int cleared;
  int has_cleared;
  char *objective;
  char *status;
  long long token_budget;
  int has_token_budget;
  long long tokens_used;
  long long created_at;
  long long updated_at;
} cai_goal_result;

static const lonejson_field cai_goal_get_arg_fields[] = {
    LONEJSON_FIELD_I64(cai_goal_get_args, ignored, "ignored")};
LONEJSON_MAP_DEFINE(cai_goal_get_args_map, cai_goal_get_args,
                    cai_goal_get_arg_fields);

static const lonejson_field cai_goal_create_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_goal_create_args, objective,
                                    "objective"),
    LONEJSON_FIELD_I64_PRESENT(cai_goal_create_args, token_budget,
                               has_token_budget, "token_budget")};
LONEJSON_MAP_DEFINE(cai_goal_create_args_map, cai_goal_create_args,
                    cai_goal_create_arg_fields);

static const lonejson_field cai_goal_update_arg_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(cai_goal_update_args, status, "status")};
LONEJSON_MAP_DEFINE(cai_goal_update_args_map, cai_goal_update_args,
                    cai_goal_update_arg_fields);

static const lonejson_field cai_goal_result_fields[] = {
    LONEJSON_FIELD_BOOL_REQ(cai_goal_result, has_goal, "has_goal"),
    LONEJSON_FIELD_BOOL_PRESENT(cai_goal_result, cleared, has_cleared,
                                "cleared"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_goal_result, objective,
                                          "objective"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(cai_goal_result, status, "status"),
    LONEJSON_FIELD_I64_PRESENT(cai_goal_result, token_budget, has_token_budget,
                               "token_budget"),
    LONEJSON_FIELD_I64_REQ(cai_goal_result, tokens_used, "tokens_used"),
    LONEJSON_FIELD_I64_REQ(cai_goal_result, created_at, "created_at"),
    LONEJSON_FIELD_I64_REQ(cai_goal_result, updated_at, "updated_at")};
LONEJSON_MAP_DEFINE(cai_goal_result_map, cai_goal_result,
                    cai_goal_result_fields);

static const char cai_goal_get_schema[] =
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
static const char cai_goal_create_schema[] =
    "{\"type\":\"object\",\"properties\":{\"objective\":{\"type\":\"string\","
    "\"description\":\"Concrete user-requested objective.\"},\"token_budget\":{"
    "\"type\":\"integer\",\"description\":\"Positive token budget; omit unless "
    "explicitly requested.\"}},"
    "\"required\":[\"objective\"],\"additionalProperties\":false}";
static const char cai_goal_update_schema[] =
    "{\"type\":\"object\",\"properties\":{\"status\":{\"type\":\"string\","
    "\"enum\":[\"complete\",\"blocked\"]}},\"required\":[\"status\"],"
    "\"additionalProperties\":false}";
static const char cai_goal_clear_schema[] =
    "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";

static long long cai_goal_now(void) { return (long long)time(NULL); }

static void cai_goal_context_cleanup(void *value) { cai_free_mem(NULL, value); }

static int cai_goal_fill_result(cai_goal_context *context,
                                cai_goal_result *result, cai_error *error) {
  cai_session_impl *session;

  if (context == NULL || context->session == NULL || result == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "goal session is required");
  }
  session = CAI_SESSION_IMPL(context->session);
  memset(result, 0, sizeof(*result));
  result->has_goal = session->goal_status != NULL;
  if (session->goal_status != NULL) {
    result->objective = cai_tool_result_strdup(session->goal_objective, error);
    if (result->objective == NULL) {
      return error != NULL ? error->code : CAI_ERR_NOMEM;
    }
    result->status = cai_tool_result_strdup(session->goal_status, error);
    if (result->status == NULL) {
      return error != NULL ? error->code : CAI_ERR_NOMEM;
    }
  }
  result->token_budget = session->goal_token_budget;
  result->has_token_budget = session->goal_has_token_budget;
  result->tokens_used = session->goal_tokens_used;
  result->created_at = session->goal_created_at;
  result->updated_at = session->goal_updated_at;
  return CAI_OK;
}

static int cai_goal_get(void *value, const void *params, void *out,
                        cai_error *error) {
  (void)params;
  return cai_goal_fill_result((cai_goal_context *)value, (cai_goal_result *)out,
                              error);
}

static int cai_goal_create(void *value, const void *params, void *out,
                           cai_error *error) {
  cai_goal_context *context;
  cai_session_impl *session;
  const cai_goal_create_args *args;
  char *objective;
  char *trimmed;
  char *end;
  long long now;

  context = (cai_goal_context *)value;
  args = (const cai_goal_create_args *)params;
  if (context == NULL || context->session == NULL || args == NULL ||
      args->objective == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "goal objective is required");
  }
  if (args->has_token_budget && args->token_budget <= 0LL) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "goal token budget must be positive");
  }
  session = CAI_SESSION_IMPL(context->session);
  if (session->goal_status != NULL &&
      strcmp(session->goal_status, "complete") != 0) {
    return cai_set_error(
        error, CAI_ERR_INVALID,
        "cannot create a new goal while an unfinished goal exists");
  }
  objective = cai_strdup(&CAI_SESSION_CLIENT_IMPL(context->session)->allocator,
                         args->objective);
  if (objective == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to store goal objective");
  }
  trimmed = objective;
  while (*trimmed != '\0' && isspace((unsigned char)*trimmed)) {
    trimmed++;
  }
  end = trimmed + strlen(trimmed);
  while (end > trimmed && isspace((unsigned char)end[-1])) {
    end--;
  }
  *end = '\0';
  if (*trimmed == '\0') {
    cai_free_mem(&CAI_SESSION_CLIENT_IMPL(context->session)->allocator,
                 objective);
    return cai_set_error(error, CAI_ERR_INVALID, "goal objective is required");
  }
  if (trimmed != objective) {
    memmove(objective, trimmed, strlen(trimmed) + 1U);
  }
  cai_free_mem(&CAI_SESSION_CLIENT_IMPL(context->session)->allocator,
               session->goal_objective);
  cai_free_mem(&CAI_SESSION_CLIENT_IMPL(context->session)->allocator,
               session->goal_status);
  session->goal_objective = objective;
  session->goal_status = cai_strdup(
      &CAI_SESSION_CLIENT_IMPL(context->session)->allocator, "active");
  if (session->goal_status == NULL) {
    cai_free_mem(&CAI_SESSION_CLIENT_IMPL(context->session)->allocator,
                 session->goal_objective);
    session->goal_objective = NULL;
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to store goal status");
  }
  now = cai_goal_now();
  session->goal_has_token_budget = args->has_token_budget;
  session->goal_token_budget = args->token_budget;
  session->goal_token_usage_baseline = session->usage.usage.total_tokens;
  session->goal_tokens_used = 0LL;
  session->goal_created_at = now;
  session->goal_updated_at = now;
  return cai_goal_fill_result(context, (cai_goal_result *)out, error);
}

static int cai_goal_update(void *value, const void *params, void *out,
                           cai_error *error) {
  cai_goal_context *context;
  cai_session_impl *session;
  const cai_goal_update_args *args;
  char *status;

  context = (cai_goal_context *)value;
  args = (const cai_goal_update_args *)params;
  if (context == NULL || context->session == NULL || args == NULL ||
      args->status == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "goal status is required");
  }
  if (strcmp(args->status, "complete") != 0 &&
      strcmp(args->status, "blocked") != 0) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "goal status must be complete or blocked");
  }
  session = CAI_SESSION_IMPL(context->session);
  if (session->goal_status == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "no active goal exists");
  }
  status = cai_strdup(&CAI_SESSION_CLIENT_IMPL(context->session)->allocator,
                      args->status);
  if (status == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM, "failed to store goal status");
  }
  cai_free_mem(&CAI_SESSION_CLIENT_IMPL(context->session)->allocator,
               session->goal_status);
  session->goal_status = status;
  session->goal_updated_at = cai_goal_now();
  return cai_goal_fill_result(context, (cai_goal_result *)out, error);
}

static int cai_goal_clear(void *value, const void *params, void *out,
                          cai_error *error) {
  cai_goal_context *context;
  cai_session_impl *session;

  (void)params;
  context = (cai_goal_context *)value;
  if (context == NULL || context->session == NULL) {
    return cai_set_error(error, CAI_ERR_INVALID, "goal session is required");
  }
  session = CAI_SESSION_IMPL(context->session);
  if (session->goal_status != NULL) {
    cai_free_mem(&CAI_SESSION_CLIENT_IMPL(context->session)->allocator,
                 session->goal_objective);
    cai_free_mem(&CAI_SESSION_CLIENT_IMPL(context->session)->allocator,
                 session->goal_status);
  }
  session->goal_objective = NULL;
  session->goal_status = NULL;
  session->goal_token_budget = 0LL;
  session->goal_has_token_budget = 0;
  session->goal_token_usage_baseline = 0LL;
  session->goal_tokens_used = 0LL;
  session->goal_created_at = 0LL;
  session->goal_updated_at = 0LL;
  if (cai_goal_fill_result(context, (cai_goal_result *)out, error) != CAI_OK) {
    return error != NULL ? error->code : CAI_ERR_NOMEM;
  }
  ((cai_goal_result *)out)->cleared = 1;
  ((cai_goal_result *)out)->has_cleared = 1;
  return CAI_OK;
}

int cai_agent_register_goal_tools(cai_agent *agent, cai_session *session,
                                  cai_error *error) {
  cai_goal_context *context;
  cai_agent_impl *impl;
  int rc;

  if (agent == NULL || agent->impl == NULL || session == NULL ||
      CAI_SESSION_IMPL(session)->agent != agent) {
    return cai_set_error(error, CAI_ERR_INVALID,
                         "goal tools require an agent session");
  }
  context = (cai_goal_context *)cai_alloc(NULL, sizeof(*context));
  if (context == NULL) {
    return cai_set_error(error, CAI_ERR_NOMEM,
                         "failed to allocate goal tool context");
  }
  context->session = session;
  impl = CAI_AGENT_IMPL(agent);
  rc = cai_tool_registry_register_lonejson_schema_owned(
      impl->tools, CAI_GOAL_GET_TOOL_NAME,
      "Get the current durable goal and its accounting.", cai_goal_get_schema,
      0, &cai_goal_get_args_map, &cai_goal_result_map, cai_goal_get, context,
      cai_goal_context_cleanup, error);
  if (rc == CAI_OK) {
    context = (cai_goal_context *)cai_alloc(NULL, sizeof(*context));
    if (context == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate goal tool context");
    }
    context->session = session;
    rc = cai_tool_registry_register_lonejson_schema_owned(
        impl->tools, CAI_GOAL_CREATE_TOOL_NAME,
        "Create a goal only when the user explicitly requests one; fails while "
        "an unfinished goal exists.",
        cai_goal_create_schema, 0, &cai_goal_create_args_map,
        &cai_goal_result_map, cai_goal_create, context,
        cai_goal_context_cleanup, error);
  }
  if (rc == CAI_OK) {
    context = (cai_goal_context *)cai_alloc(NULL, sizeof(*context));
    if (context == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate goal tool context");
    }
    context->session = session;
    rc = cai_tool_registry_register_lonejson_schema_owned(
        impl->tools, CAI_GOAL_UPDATE_TOOL_NAME,
        "Set an existing goal to complete or blocked only when that status is "
        "true.",
        cai_goal_update_schema, 0, &cai_goal_update_args_map,
        &cai_goal_result_map, cai_goal_update, context,
        cai_goal_context_cleanup, error);
  }
  if (rc == CAI_OK) {
    context = (cai_goal_context *)cai_alloc(NULL, sizeof(*context));
    if (context == NULL) {
      return cai_set_error(error, CAI_ERR_NOMEM,
                           "failed to allocate goal tool context");
    }
    context->session = session;
    rc = cai_tool_registry_register_lonejson_schema_owned(
        impl->tools, CAI_GOAL_CLEAR_TOOL_NAME,
        "Clear the current goal without marking it complete.",
        cai_goal_clear_schema, 0, &cai_goal_get_args_map, &cai_goal_result_map,
        cai_goal_clear, context, cai_goal_context_cleanup, error);
  }
  return rc;
}
