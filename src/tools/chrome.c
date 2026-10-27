#include "../cai_chrome_protocol.h"
#include <ctype.h>
#include <math.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

/* One deliberately small function schema works with Responses and Chat
 * Completions. CDP parameters remain arbitrary validated JSON objects. */
static const char chrome_schema[] =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"tabs\",\"open\",\"claim\","
    "\"navigate\",\"snapshot\",\"screenshot\",\"click\",\"type\",\"key\","
    "\"scroll\",\"evaluate\",\"cdp\",\"keep\",\"close\"]},"
    "\"tab_id\":{\"type\":\"integer\"},\"url\":{\"type\":\"string\"},"
    "\"text\":{\"type\":\"string\"},\"x\":{\"type\":\"number\"},\"y\":{"
    "\"type\":\"number\"},";
static const char chrome_schema_tail[] =
    "\"delta_y\":{\"type\":\"number\"},\"method\":{\"type\":\"string\"},"
    "\"params\":{\"type\":\"object\"}},\"required\":[\"action\"],"
    "\"additionalProperties\":false}";
static const char chrome_description[] =
    "Control the user's Chrome through the installed ChatGPT extension. tabs "
    "lists "
    "user tabs; open creates a blank tab and optionally navigates url; claim "
    "owns "
    "and attaches tab_id. Claim an existing tab before other actions. navigate "
    "uses "
    "url. snapshot returns visible page text and links; screenshot attaches a "
    "PNG "
    "image. click uses viewport x/y; type inserts text; key uses a CDP key "
    "name; ";
static const char chrome_description_tail[] =
    "scroll uses x/y and delta_y. evaluate runs text as JavaScript; cdp uses "
    "method "
    "and params for advanced browser operations. keep marks a tab deliverable "
    "so "
    "it survives turn cleanup; close closes only an owned tab. Re-observe "
    "after "
    "navigation: page loading is asynchronous. All actions except tabs/open "
    "need tab_id.";
typedef struct chrome_args {
  char *action, *url, *text, *method;
  int has_tab, has_x, has_y, has_delta;
  int64_t tab_id;
  double x, y, delta_y;
  lonejson_json_value params;
} chrome_args;
static const lonejson_field args_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_args, action, "action"),
    LONEJSON_FIELD_I64_PRESENT(chrome_args, tab_id, has_tab, "tab_id"),
    LONEJSON_FIELD_STRING_ALLOC(chrome_args, url, "url"),
    LONEJSON_FIELD_STRING_ALLOC(chrome_args, text, "text"),
    LONEJSON_FIELD_STRING_ALLOC(chrome_args, method, "method"),
    LONEJSON_FIELD_F64_PRESENT(chrome_args, x, has_x, "x"),
    LONEJSON_FIELD_F64_PRESENT(chrome_args, y, has_y, "y"),
    LONEJSON_FIELD_F64_PRESENT(chrome_args, delta_y, has_delta, "delta_y"),
    CAI_CHROME_JSON_FIELD(chrome_args, params, "params")};
LONEJSON_MAP_DEFINE(args_map, chrome_args, args_fields);
typedef struct chrome_target {
  int64_t tab;
} chrome_target;
static const lonejson_field target_fields[] = {
    LONEJSON_FIELD_I64_REQ(chrome_target, tab, "tabId")};
LONEJSON_MAP_DEFINE(target_map, chrome_target, target_fields);
typedef struct chrome_operation {
  const char *session, *turn, *context;
  int has_tab;
  int64_t tab;
  chrome_target target;
  const char *method, *status;
  lonejson_json_value params;
} chrome_operation;
static const lonejson_field operation_base[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_operation, session, "session_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_operation, turn, "turn_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_operation, context,
                                    "session_context"),
    LONEJSON_FIELD_I64_PRESENT(chrome_operation, tab, has_tab, "tabId"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(chrome_operation, status, "status")};
LONEJSON_MAP_DEFINE(operation_map, chrome_operation, operation_base);
static const lonejson_field cdp_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_operation, session, "session_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_operation, turn, "turn_id"),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_operation, context,
                                    "session_context"),
    LONEJSON_FIELD_OBJECT(chrome_operation, target, "target", &target_map),
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_operation, method, "method"),
    /* The extension passes commandParams to chrome.debugger.sendCommand.
     * "params" here would be ignored; it belongs to the JSON-RPC envelope. */
    CAI_CHROME_JSON_FIELD(chrome_operation, params, "commandParams")};
LONEJSON_MAP_DEFINE(cdp_map, chrome_operation, cdp_fields);
typedef struct chrome_cdp_params {
  const char *url, *expression, *text, *type, *button, *key;
  int has_coords, has_delta, has_click;
  int64_t click_count;
  double x, y, delta_y;
  int return_by_value, await_promise, has_evaluate;
  int has_key_code;
  int64_t key_code;
  double delta_x;
} chrome_cdp_params;
static const lonejson_field params_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(chrome_cdp_params, url, "url"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(chrome_cdp_params, expression,
                                          "expression"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(chrome_cdp_params, text, "text"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(chrome_cdp_params, type, "type"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(chrome_cdp_params, button, "button"),
    LONEJSON_FIELD_STRING_ALLOC_OMIT_NULL(chrome_cdp_params, key, "key"),
    LONEJSON_FIELD_F64_PRESENT(chrome_cdp_params, x, has_coords, "x"),
    LONEJSON_FIELD_F64_PRESENT(chrome_cdp_params, y, has_coords, "y"),
    LONEJSON_FIELD_F64_PRESENT(chrome_cdp_params, delta_y, has_delta, "deltaY"),
    LONEJSON_FIELD_I64_PRESENT(chrome_cdp_params, click_count, has_click,
                               "clickCount"),
    LONEJSON_FIELD_BOOL_PRESENT(chrome_cdp_params, return_by_value,
                                has_evaluate, "returnByValue"),
    LONEJSON_FIELD_BOOL_PRESENT(chrome_cdp_params, await_promise, has_evaluate,
                                "awaitPromise"),
    LONEJSON_FIELD_I64_PRESENT(chrome_cdp_params, key_code, has_key_code,
                               "windowsVirtualKeyCode"),
    LONEJSON_FIELD_F64_PRESENT(chrome_cdp_params, delta_x, has_delta,
                               "deltaX")};
LONEJSON_MAP_DEFINE(params_map, chrome_cdp_params, params_fields);
typedef struct chrome_tab_result {
  int64_t id;
} chrome_tab_result;
static const lonejson_field tab_fields[] = {
    LONEJSON_FIELD_I64_REQ(chrome_tab_result, id, "id")};
LONEJSON_MAP_DEFINE(tab_map, chrome_tab_result, tab_fields);
typedef struct chrome_image {
  char *data;
} chrome_image;
static const lonejson_field image_fields[] = {
    LONEJSON_FIELD_STRING_ALLOC_REQ(chrome_image, data, "data")};
LONEJSON_MAP_DEFINE(image_map, chrome_image, image_fields);
typedef struct chrome_result {
  lonejson_json_value result;
  char *status;
} chrome_result;
static const lonejson_field result_fields[] = {
    CAI_CHROME_JSON_FIELD(chrome_result, result, "result"),
    LONEJSON_FIELD_STRING_ALLOC(chrome_result, status, "status")};
LONEJSON_MAP_DEFINE(result_map, chrome_result, result_fields);
static int operation(cai_chrome *c, chrome_operation *op, const char *method,
                     const char *params, char **out, cai_error *error) {
  char *json;
  int rc;
  op->session = c->session_id;
  op->turn = c->turn_id;
  op->context = "live";
  if (params != NULL) {
    rc = cai_chrome_value_buffer(&op->params, params, error);
    if (rc != CAI_OK)
      return rc;
  }
  json = cai_chrome_serialize(params != NULL ? &cdp_map : &operation_map, op,
                              error);
  CAI_LJ_CHROME->json_value_cleanup(CAI_LJ_CHROME, &op->params);
  if (json == NULL)
    return CAI_ERR_PROTOCOL;
  c->used = 1;
  rc = cai_chrome_request(c, method, json, out, error);
  cai_free_mem(NULL, json);
  return rc;
}
static int chrome_callback(void *context, const void *arguments, void *output,
                           cai_error *error) {
  cai_chrome *c = context;
  const chrome_args *a = arguments;
  chrome_result *result = output;
  chrome_operation op;
  chrome_cdp_params p;
  lonejson_error je;
  char *params = NULL, *reply = NULL;
  const char *method = NULL;
  int rc = CAI_OK;
  memset(&op, 0, sizeof(op));
  memset(&p, 0, sizeof(p));
  lonejson_error_init(&je);
  cai_free_mem(NULL, c->pending_image);
  c->pending_image = NULL;
  if (c->session_id == NULL || c->turn_id == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Begin a Chrome turn before using browser tools");
  if (a->action == NULL || a->action[0] == '\0')
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Chrome tool requires a non-empty action string");
  if (strcmp(a->action, "tabs") == 0)
    method = "getUserTabs";
  else if (strcmp(a->action, "open") == 0)
    method = "createTab";
  else {
    if (!a->has_tab || a->tab_id < 0LL || a->tab_id > 2147483647LL)
      return cai_set_error(error, CAI_ERR_INVALID,
                           "Chrome action requires a non-negative tab_id");
    op.tab = a->tab_id;
    op.has_tab = 1;
    op.target.tab = a->tab_id;
    if (strcmp(a->action, "claim") == 0)
      method = "claimUserTab";
    else if (strcmp(a->action, "keep") == 0) {
      method = "markTab";
      op.status = "deliverable";
    } else if (strcmp(a->action, "close") == 0) {
      method = "executeCdp";
      op.method = "Page.close";
    } else if (strcmp(a->action, "navigate") == 0) {
      if (a->url == NULL || (strncmp(a->url, "https://", 8U) != 0 &&
                             strncmp(a->url, "http://", 7U) != 0))
        return cai_set_error(
            error, CAI_ERR_INVALID,
            "Chrome navigate requires an http:// or https:// URL");
      method = "executeCdp";
      op.method = "Page.navigate";
      p.url = a->url;
    } else if (strcmp(a->action, "snapshot") == 0 ||
               strcmp(a->action, "evaluate") == 0) {
      method = "executeCdp";
      op.method = "Runtime.evaluate";
      if (strcmp(a->action, "evaluate") == 0 && a->text == NULL)
        return cai_set_error(error, CAI_ERR_INVALID,
                             "Chrome evaluate requires JavaScript in text");
      p.expression =
          strcmp(a->action, "evaluate") == 0
              ? a->text
              : "JSON.stringify({url:location.href,title:document.title,text:("
                "document.body?.innerText||'').slice(0,24000),links:Array.from("
                "document.querySelectorAll('a[href]')).slice(0,100).map(a=>({"
                "text:a.innerText.slice(0,200),href:a.href}))})";
      p.return_by_value = 1;
      p.await_promise = 1;
      p.has_evaluate = 1;
    } else if (strcmp(a->action, "screenshot") == 0) {
      method = "executeCdp";
      op.method = "Page.captureScreenshot";
    } else if (strcmp(a->action, "type") == 0) {
      if (a->text == NULL)
        return cai_set_error(error, CAI_ERR_INVALID,
                             "Chrome type requires text");
      method = "executeCdp";
      op.method = "Input.insertText";
      p.text = a->text;
    } else if (strcmp(a->action, "key") == 0) {
      if (a->text == NULL || a->text[0] == '\0')
        return cai_set_error(error, CAI_ERR_INVALID,
                             "Chrome key requires a CDP key name in text");
      method = "executeCdp";
      op.method = "Input.dispatchKeyEvent";
      p.type = "keyDown";
      p.key = a->text;
      if (strcmp(a->text, "Enter") == 0)
        p.key_code = 13LL;
      else if (strcmp(a->text, "Tab") == 0)
        p.key_code = 9LL;
      else if (strcmp(a->text, "Escape") == 0)
        p.key_code = 27LL;
      else if (strcmp(a->text, "Backspace") == 0)
        p.key_code = 8LL;
      else if (strcmp(a->text, "Delete") == 0)
        p.key_code = 46LL;
      else if (strcmp(a->text, "ArrowLeft") == 0)
        p.key_code = 37LL;
      else if (strcmp(a->text, "ArrowUp") == 0)
        p.key_code = 38LL;
      else if (strcmp(a->text, "ArrowRight") == 0)
        p.key_code = 39LL;
      else if (strcmp(a->text, "ArrowDown") == 0)
        p.key_code = 40LL;
      p.has_key_code = p.key_code != 0LL;
    } else if (strcmp(a->action, "click") == 0 ||
               strcmp(a->action, "scroll") == 0) {
      if (!a->has_x || !a->has_y || !isfinite(a->x) || !isfinite(a->y) ||
          a->x < 0.0 || a->y < 0.0)
        return cai_set_error(error, CAI_ERR_INVALID,
                             "Chrome click/scroll requires finite, "
                             "non-negative viewport x and y");
      method = "executeCdp";
      op.method = "Input.dispatchMouseEvent";
      p.x = a->x;
      p.y = a->y;
      p.has_coords = 1;
      if (strcmp(a->action, "click") == 0) {
        p.type = "mousePressed";
        p.button = "left";
        p.has_click = 1;
        p.click_count = 1LL;
      } else {
        if (!a->has_delta || !isfinite(a->delta_y))
          return cai_set_error(error, CAI_ERR_INVALID,
                               "Chrome scroll requires finite delta_y");
        p.type = "mouseWheel";
        p.delta_y = a->delta_y;
        p.has_delta = 1;
      }
    } else if (strcmp(a->action, "cdp") == 0) {
      if (a->method == NULL || a->method[0] == '\0')
        return cai_set_error(error, CAI_ERR_INVALID,
                             "Chrome cdp requires a method");
      method = "executeCdp";
      op.method = a->method;
      params = cai_chrome_value(&a->params, error);
      if (params == NULL || params[0] != '{') {
        cai_free_mem(NULL, params);
        return cai_set_error(error, CAI_ERR_INVALID,
                             "Chrome cdp requires object params");
      }
    } else
      return cai_set_error(error, CAI_ERR_INVALID, "Unknown Chrome action");
  }
  /* Validate open's URL before creating a tab, to avoid partial side effects.
   */
  if (strcmp(a->action, "open") == 0 && a->url != NULL &&
      strncmp(a->url, "https://", 8U) != 0 &&
      strncmp(a->url, "http://", 7U) != 0)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Chrome open URL must use http:// or https://");
  if (strcmp(method, "executeCdp") == 0 && params == NULL) {
    params = cai_chrome_serialize(&params_map, &p, error);
    if (params == NULL)
      return CAI_ERR_NOMEM;
  }
  rc = operation(c, &op, method, params, &reply, error);
  if (rc == CAI_OK &&
      (strcmp(a->action, "open") == 0 || strcmp(a->action, "claim") == 0)) {
    chrome_tab_result tab;
    memset(&tab, 0, sizeof(tab));
    if (CAI_LJ_CHROME->parse_cstr(CAI_LJ_CHROME, &tab_map, &tab, reply, &je) !=
        LONEJSON_STATUS_OK)
      rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "Chrome returned an invalid tab", je.message);
    if (rc == CAI_OK) {
      char *attached = NULL;
      op.tab = tab.id;
      op.has_tab = 1;
      op.target.tab = tab.id;
      rc = operation(c, &op, "attach", NULL, &attached, error);
      cai_free_mem(NULL, attached);
    }
    if (rc == CAI_OK && strcmp(a->action, "open") == 0 && a->url != NULL) {
      char *navigated = NULL;
      p.url = a->url;
      op.method = "Page.navigate";
      cai_free_mem(NULL, params);
      params = cai_chrome_serialize(&params_map, &p, error);
      if (params == NULL)
        rc = CAI_ERR_NOMEM;
      else
        rc = operation(c, &op, "executeCdp", params, &navigated, error);
      cai_free_mem(NULL, navigated);
    }
  }
  if (rc == CAI_OK &&
      (strcmp(a->action, "click") == 0 || strcmp(a->action, "key") == 0)) {
    cai_free_mem(NULL, reply);
    reply = NULL;
    p.type = strcmp(a->action, "click") == 0 ? "mouseReleased" : "keyUp";
    cai_free_mem(NULL, params);
    params = cai_chrome_serialize(&params_map, &p, error);
    if (params == NULL)
      rc = CAI_ERR_NOMEM;
    else
      rc = operation(c, &op, "executeCdp", params, &reply, error);
  }
  if (rc == CAI_OK && (strcmp(a->action, "screenshot") == 0 ||
                       (op.method != NULL &&
                        strcmp(op.method, "Page.captureScreenshot") == 0))) {
    chrome_image image;
    size_t n;
    memset(&image, 0, sizeof(image));
    if (CAI_LJ_CHROME->parse_cstr(CAI_LJ_CHROME, &image_map, &image, reply,
                                  &je) != LONEJSON_STATUS_OK)
      rc = cai_set_error_detail(error, CAI_ERR_PROTOCOL,
                                "Chrome screenshot contains no image data",
                                je.message);
    else if (image.data == NULL) {
      rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                         "Chrome screenshot contains no image data");
    } else {
      n = strlen(image.data);
      if (n < 12U || n % 4U != 0U ||
          strncmp(image.data, "iVBORw0KGgo", 11U) != 0 ||
          strspn(image.data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvw"
                             "xyz0123456789+/=") != n)
        rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                           "Chrome screenshot is not a base64 PNG");
      else {
        unsigned char *decoded;
        int decoded_length;
        size_t padding = image.data[n - 1U] == '='
                             ? (image.data[n - 2U] == '=' ? 2U : 1U)
                             : 0U;
        decoded = cai_alloc(NULL, n / 4U * 3U + 1U);
        if (decoded == NULL)
          rc = cai_set_error(error, CAI_ERR_NOMEM,
                             "Cannot validate Chrome screenshot");
        else {
          decoded_length = EVP_DecodeBlock(
              decoded, (const unsigned char *)image.data, (int)n);
          if (strchr(image.data, '=') !=
                  (padding != 0U ? image.data + n - padding : NULL) ||
              decoded_length < 8 ||
              memcmp(decoded, "\x89PNG\r\n\x1a\n", 8U) != 0)
            rc = cai_set_error(error, CAI_ERR_PROTOCOL,
                               "Chrome screenshot has invalid PNG/base64 data");
          cai_free_mem(NULL, decoded);
        }
        if (rc == CAI_OK)
          c->pending_image = cai_alloc(NULL, n + 23U);
        if (rc == CAI_OK && c->pending_image == NULL)
          rc = cai_set_error(error, CAI_ERR_NOMEM,
                             "Cannot allocate typed Chrome screenshot");
        else if (rc == CAI_OK)
          snprintf(c->pending_image, n + 23U, "data:image/png;base64,%s",
                   image.data);
      }
    }
    CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &image_map, &image);
    if (rc == CAI_OK) {
      cai_free_mem(NULL, reply);
      reply = cai_strdup(
          NULL, "{\"image\":\"attached\",\"mime_type\":\"image/png\"}");
    }
  }
  if (rc == CAI_OK) {
    rc = reply != NULL ? cai_chrome_value_buffer(&result->result, reply, error)
                       : cai_set_error(error, CAI_ERR_NOMEM,
                                       "Cannot allocate Chrome tool result");
    if (rc == CAI_OK) {
      result->status = cai_strdup(NULL, "ok");
      if (result->status == NULL)
        rc = CAI_ERR_NOMEM;
    }
  }
  cai_free_mem(NULL, params);
  cai_free_mem(NULL, reply);
  return rc;
}
static int chrome_deliver(void *context, const char *call_id,
                          cai_response_create_params *params,
                          const lonejson_spooled *output, int *delivered,
                          cai_error *error) {
  cai_chrome *c = context;
  int rc;
  (void)output;
  *delivered = 0;
  if (c->pending_image == NULL)
    return CAI_OK;
  rc = cai_response_create_params_add_function_call_output_image_url(
      params, call_id, c->pending_image, "high", error);
  if (rc == CAI_OK) {
    cai_free_mem(NULL, c->pending_image);
    c->pending_image = NULL;
    *delivered = 1;
  }
  return rc;
}
static int chrome_raw_callback(void *context, const char *arguments,
                               cai_sink *output, cai_error *error) {
  chrome_args args;
  chrome_result result;
  lonejson_error je;
  char *json = NULL;
  int rc;
  memset(&args, 0, sizeof(args));
  memset(&result, 0, sizeof(result));
  lonejson_error_init(&je);
  CAI_LJ_CHROME->json_value_init(CAI_LJ_CHROME, &args.params);
  if (lonejson_json_value_enable_parse_capture(&args.params, &je) !=
      LONEJSON_STATUS_OK)
    rc = cai_set_error(error, CAI_ERR_NOMEM,
                       "Cannot initialize Chrome tool parameters");
  else if (CAI_LJ_CHROME->parse_cstr(CAI_LJ_CHROME, &args_map, &args, arguments,
                                     &je) != LONEJSON_STATUS_OK)
    rc = cai_set_error_detail(error, CAI_ERR_INVALID,
                              "Invalid Chrome tool arguments", je.message);
  else
    rc = chrome_callback(context, &args, &result, error);
  if (rc == CAI_OK) {
    json = cai_chrome_serialize(&result_map, &result, error);
    if (json == NULL)
      rc = CAI_ERR_NOMEM;
    else
      rc = cai_sink_write(output, json, strlen(json), error);
  }
  CAI_LJ_CHROME->cleanup(CAI_LJ_CHROME, &args_map, &args);
  CAI_LJ_CHROME->json_value_cleanup(CAI_LJ_CHROME, &result.result);
  cai_free_mem(NULL, result.status);
  cai_free_mem(NULL, json);
  return rc;
}
int cai_agent_register_chrome_tool(cai_agent *agent, cai_chrome *chrome,
                                   cai_error *error) {
  cai_agent_impl *impl = CAI_AGENT_IMPL(agent);
  char schema[1600];
  char description[900];
  int rc;
  if (impl == NULL || chrome == NULL)
    return cai_set_error(error, CAI_ERR_INVALID,
                         "Agent and Chrome connection are required");
  snprintf(schema, sizeof(schema), "%s%s", chrome_schema, chrome_schema_tail);
  snprintf(description, sizeof(description), "%s%s", chrome_description,
           chrome_description_tail);
  rc =
      cai_tool_registry_register_raw(impl->tools, "chrome", description, schema,
                                     0, chrome_raw_callback, chrome, error);
  if (rc == CAI_OK)
    rc = cai_tool_registry_set_result_delivery(impl->tools, "chrome",
                                               chrome_deliver, error);
  return rc;
}
