#include <assert.h>
#include <cai/skills.h>
#include <stdlib.h>
#include <string.h>

typedef struct reader {
  const char *text;
  size_t size, position, chunk;
  int fail;
} reader;
static size_t read_bytes(void *context, void *bytes, size_t capacity,
                         cai_error *error) {
  reader *r = context;
  size_t n = r->size - r->position;
  if (r->fail && r->position > 7U) {
    error->code = CAI_ERR_TRANSPORT;
    return 0U;
  }
  if (n > capacity)
    n = capacity;
  if (n > r->chunk)
    n = r->chunk;
  memcpy(bytes, r->text + r->position, n);
  r->position += n;
  return n;
}
static int parse(reader *r, cai_skill_metadata **metadata, cai_error *error) {
  cai_source_callbacks callbacks;
  cai_source *source;
  int rc;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.context = r;
  callbacks.read = read_bytes;
  assert(cai_source_from_callbacks(&callbacks, &source, error) == CAI_OK);
  rc = cai_skill_metadata_open(source, "/packages/fallback", metadata, error);
  source->close(source);
  return rc;
}
int main(void) {
  static const char *const valid[] = {
      "---\nname: 'quoted-name'\ndescription: \"Do: work # carefully\"\n---\n# "
      "Body: [not YAML",
      "---\r\nname: quoted-name\r\ndescription: >-\r\n  Do: work # "
      "carefully\r\n---\r\n",
      "---\nname: quoted-name\ndescription: |-\n  Do: work\n  # "
      "carefully\nmetadata:\n  anything: [one, two]\n---\n",
      "---\nname: quoted-name\nsummary: &description 'Do: work # "
      "carefully'\ndescription: *description\n---",
      "\xef\xbb\xbf---\nname: quoted-name\ndescription: 'Do: work # "
      "carefully'\n--- # end\n"};
  static const char *const invalid[] = {
      "name: missing-fence\ndescription: invalid\n",
      "---\nname: unclosed\ndescription: no closer\n",
      "---\nname: duplicate\nname: another\ndescription: invalid\n---\n",
      "---\nname: duplicate\ndescription: one\ndescription: two\n---\n",
      "---\nname: '../escape'\ndescription: invalid\n---\n",
      "---\nname: []\ndescription: invalid\n---\n",
      "---\nname: missing-description\n---\n",
      "---\nname: empty\ndescription: \"   \"\n---\n",
      "---\nname: bad\ndescription: {nested: object}\n---\n",
      "---\nname: \"nul\\0name\"\ndescription: invalid\n---\n",
      "---\nname: bad\ndescription: !!int 123\n---\n",
      "---\nname: bad\ndescription: \"unterminated\n---\n"};
  reader r;
  cai_skill_metadata *metadata;
  cai_error error;
  size_t i, chunk;
  char *large;
  const char *prefix = "---\nname: small\ndescription: Small header.\n---\n";
  cai_error_init(&error);
  memset(&r, 0, sizeof(r));
  for (i = 0U; i < sizeof(valid) / sizeof(valid[0]); i++)
    for (chunk = 1U; chunk <= 4096U; chunk *= 16U) {
      r.text = valid[i];
      r.size = strlen(r.text);
      r.position = 0U;
      r.chunk = chunk;
      assert(parse(&r, &metadata, &error) == CAI_OK);
      assert(strcmp(metadata->name(metadata), "quoted-name") == 0);
      assert(strcmp(metadata->description(metadata), "Do: work # carefully") ==
             0);
      metadata->close(metadata);
    }
  for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    r.text = invalid[i];
    r.size = strlen(r.text);
    r.position = 0U;
    r.chunk = 2U;
    assert(parse(&r, &metadata, &error) == CAI_ERR_INVALID && metadata == NULL);
    cai_error_cleanup(&error);
  }
  r.text = "---\ndescription: Fallback name.\n---\n";
  r.size = strlen(r.text);
  r.position = 0U;
  r.chunk = 3U;
  assert(parse(&r, &metadata, &error) == CAI_OK);
  assert(strcmp(metadata->name(metadata), "fallback") == 0);
  metadata->close(metadata);
  r.text = valid[0];
  r.size = strlen(r.text);
  r.position = 0U;
  r.chunk = 1U;
  r.fail = 1;
  assert(parse(&r, &metadata, &error) == CAI_ERR_TRANSPORT);
  cai_error_cleanup(&error);
  r.position = 0U;
  assert(parse(&r, &metadata, NULL) == CAI_ERR_TRANSPORT);
  r.fail = 0;
  large = malloc(2U * 1024U * 1024U);
  assert(large != NULL);
  memset(large, 'x', 2U * 1024U * 1024U);
  memcpy(large, prefix, strlen(prefix));
  r.text = large;
  r.size = 2U * 1024U * 1024U;
  r.position = 0U;
  r.chunk = 4096U;
  assert(parse(&r, &metadata, &error) == CAI_OK);
  assert(r.position <= 4096U);
  metadata->close(metadata);
  memset(large, 'x', 2U * 1024U * 1024U);
  memcpy(large, "---\nmetadata: ", 14U);
  r.position = 0U;
  assert(parse(&r, &metadata, &error) == CAI_ERR_INVALID);
  cai_error_cleanup(&error);
  free(large);
  return 0;
}
