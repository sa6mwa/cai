#include <assert.h>
#include <string.h>
#include <yaml.h>

int main(void) {
  const char *input =
      "name: 'example'\ndescription: >-\n  A folded\n  description.\n";
  yaml_parser_t parser;
  yaml_document_t document;
  yaml_node_t *root, *description;
  yaml_node_pair_t *pair;
  assert(yaml_parser_initialize(&parser));
  yaml_parser_set_input_string(&parser, (const unsigned char *)input,
                               strlen(input));
  assert(yaml_parser_load(&parser, &document));
  root = yaml_document_get_root_node(&document);
  assert(root != NULL && root->type == YAML_MAPPING_NODE);
  pair = root->data.mapping.pairs.start + 1;
  description = yaml_document_get_node(&document, pair->value);
  assert(description != NULL && description->type == YAML_SCALAR_NODE);
  assert(strcmp((const char *)description->data.scalar.value,
                "A folded description.") == 0);
  yaml_document_delete(&document);
  yaml_parser_delete(&parser);
  return 0;
}
