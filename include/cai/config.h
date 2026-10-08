#ifndef CAI_CONFIG_H
#define CAI_CONFIG_H

#include <cai/agent_runtime.h>
#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** INI values are stored in owned, validated form. Lists repeat their key. */
typedef enum cai_config_type {
  CAI_CONFIG_STRING = 0,
  CAI_CONFIG_BOOL = 1,
  CAI_CONFIG_INTEGER = 2,
  CAI_CONFIG_REAL = 3,
  CAI_CONFIG_STRINGS = 4
} cai_config_type;

#define CAI_CONFIG_PATH 1U
#define CAI_CONFIG_SECRET 2U
#define CAI_CONFIG_SIGNED 4U

/** Declared field. Strings are copied by open; section "" is global.
 * NULL default_value means optional/inherited; choices uses | separators.
 * Path values resolve ~ and file-relative paths only when resolved explicitly.
 * Integer defaults and values must fit signed 64-bit; unsigned fields reject
 * negatives. Real numbers use a dot decimal separator independently of the
 * application's numeric locale. Native field types, choices and ranges
 * remain enforced in extended/selected schemas. Secret values are never
 * included in diagnostics or templates. */
typedef struct cai_config_field {
  const char *section;
  const char *key;
  cai_config_type type;
  const char *default_value;
  const char *description;
  const char *choices;
  unsigned int flags;
} cai_config_field;

typedef struct cai_config cai_config;
/** Receiver for strict INI parsing and settings. Borrowed values remain valid
 * until successful load, set, resolve_path or close. This owner-thread object
 * has no implicit environment, authentication or transport side effects. */
struct cai_config {
  size_t (*field_count)(const cai_config *self);
  const cai_config_field *(*field_at)(const cai_config *self, size_t index);
  int (*load)(cai_config *self, cai_source *source, const char *origin,
              cai_error *error);
  int (*load_file)(cai_config *self, const char *path, cai_error *error);
  const char *(*get)(const cai_config *self, const char *section,
                     const char *key);
  int (*get_bool)(const cai_config *self, const char *section, const char *key,
                  int fallback);
  long long (*get_integer)(const cai_config *self, const char *section,
                           const char *key, long long fallback);
  double (*get_real)(const cai_config *self, const char *section,
                     const char *key, double fallback);
  const char *const *(*get_list)(const cai_config *self, const char *section,
                                 const char *key, size_t *count);
  int (*resolve_path)(cai_config *self, const char *section, const char *key,
                      const char **out, cai_error *error);
  int (*set_default)(cai_config *self, const char *section, const char *key,
                     const char *value, cai_error *error);
  int (*set)(cai_config *self, const char *section, const char *key,
             const char *value, cai_error *error);
  int (*apply_environment)(const cai_config *self, cai_error *error);
  int (*write_template)(const cai_config *self, cai_sink *sink,
                        cai_error *error);
  int (*create_template_file)(const cai_config *self, const char *path,
                              cai_error *error);
  int (*apply_client)(const cai_config *self, cai_client_config *out,
                      cai_error *error);
  int (*apply_agent)(const cai_config *self, cai_agent_config *out,
                     cai_error *error);
  int (*apply_runtime)(const cai_config *self, cai_agent_runtime_config *out,
                       cai_error *error);
  int (*apply_terminal)(const cai_config *self, cai_terminal_tool_config *out,
                        cai_error *error);
  void (*close)(cai_config *self);
  void *impl;
};

/** Open with SDK schemas and optional host fields. An extension may override
 * a standard field's template default/description or narrow its choices,
 * retaining its native type/range. NULL choices inherit native choices.
 * Empty open contains no explicit values. No configuration files are read. */
int cai_config_open(const cai_config_field *extensions, size_t count,
                    cai_config **out, cai_error *error);
/** Open with exactly the supplied fields, copied and validated. Hosts can
 * select supported fields from an SDK receiver's field_at/field_count and add
 * their own declarations. [env] remains available in every schema. */
int cai_config_open_schema(const cai_config_field *fields, size_t count,
                           cai_config **out, cai_error *error);
/** Borrow schema metadata until set_default or close. NULL/out-of-range is
 * accepted and returns zero/NULL. */
size_t cai_config_field_count(const cai_config *self);
const cai_config_field *cai_config_field_at(const cai_config *self,
                                            size_t index);
/** Incrementally read source, borrowing it without closing. Entire-file
 * buffering is avoided; resulting settings are materialized and owned.
 * Failure preserves prior settings. Lines/values are limited to 1 MiB.
 * origin labels diagnostics and supplies the base for relative paths. */
int cai_config_load(cai_config *self, cai_source *source, const char *origin,
                    cai_error *error);
/** Explicit regular-file read, with the same transaction/origin semantics.
 * Directories, devices and FIFOs are rejected without blocking. */
int cai_config_load_file(cai_config *self, const char *path, cai_error *error);
/** Borrow explicit scalar or last list value, NULL when absent. */
const char *cai_config_get(const cai_config *self, const char *section,
                           const char *key);
/** Borrow list values, valid until mutation/close. Missing returns zero count.
 */
const char *const *cai_config_get_list(const cai_config *self,
                                       const char *section, const char *key,
                                       size_t *count);
/** Validated scalar replacement or list append; no environment changes.
 * Values use the same 1 MiB bound as parsed configuration values. */
int cai_config_set(cai_config *self, const char *section, const char *key,
                   const char *value, cai_error *error);
/** Change only the documented template default, not an explicit setting. */
int cai_config_set_default(cai_config *self, const char *section,
                           const char *key, const char *value,
                           cai_error *error);
/** Typed accessors return supplied fallback for missing fields. */
int cai_config_get_bool(const cai_config *self, const char *section,
                        const char *key, int fallback);
long long cai_config_get_integer(const cai_config *self, const char *section,
                                 const char *key, long long fallback);
double cai_config_get_real(const cai_config *self, const char *section,
                           const char *key, double fallback);
/** Resolve and retain an explicitly supplied path using current HOME.
 * Empty/missing resolves to NULL (automatic). Relative paths are based on
 * origin's parent, or cwd for a source without an absolute filename. */
int cai_config_resolve_path(cai_config *self, const char *section,
                            const char *key, const char **out,
                            cai_error *error);
/** Apply [env] explicitly before worker threads start. Existing variables,
 * including empty values, win unless global config_env_has_precedence=true.
 * Validation/allocation precede changes; failure attempts rollback. Process
 * environment mutation must be serialized by the embedding application. */
int cai_config_apply_environment(const cai_config *self, cai_error *error);
/** Emit documented sections and commented defaults, never explicit settings
 * or live environment/secret values. Globals precede sections even when
 * declared later in the schema. Caller owns sink. */
int cai_config_write_template(const cai_config *self, cai_sink *sink,
                              cai_error *error);
/** Atomically create a missing absolute template filename, mode 0600,
 * creating missing parents mode 0700. Existing files are never replaced. */
int cai_config_create_template_file(const cai_config *self, const char *path,
                                    cai_error *error);
/** Explicit fields overlay caller-initialized records; missing fields and
 * host objects/callbacks are preserved. Strings/lists borrow the config.
 * Resolve declared paths explicitly before applying records. [response] and
 * [session_limits] configure low-level agents; [agent]/[runtime]/[review]
 * configure Smith runtimes. A host may select a smaller supported schema.
 * client.provider is host policy: select that provider's preset and supply
 * its auth objects before apply_client. Empty base_url/api_key_env/
 * metadata_provider inherit that preset's values. Callbacks must not
 * mutate/close the receiver during load/write_template. */
int cai_config_apply_client(const cai_config *self, cai_client_config *out,
                            cai_error *error);
int cai_config_apply_agent(const cai_config *self, cai_agent_config *out,
                           cai_error *error);
int cai_config_apply_runtime(const cai_config *self,
                             cai_agent_runtime_config *out, cai_error *error);
int cai_config_apply_terminal(const cai_config *self,
                              cai_terminal_tool_config *out, cai_error *error);
/** Destroy configuration and all owned values. NULL is accepted. */
void cai_config_close(cai_config *self);

#ifdef __cplusplus
}
#endif
#endif
