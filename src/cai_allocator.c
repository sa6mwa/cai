#include "cai_internal.h"

#include <stdlib.h>
#include <string.h>

void *cai_alloc(const cai_allocator *allocator, size_t size) {
  if (size == 0U) {
    size = 1U;
  }
  if (allocator != NULL && allocator->malloc_fn != NULL) {
    return allocator->malloc_fn(allocator->context, size);
  }
  return malloc(size);
}

void *cai_realloc_mem(const cai_allocator *allocator, void *ptr, size_t size) {
  if (size == 0U) {
    size = 1U;
  }
  if (allocator != NULL && allocator->realloc_fn != NULL) {
    return allocator->realloc_fn(allocator->context, ptr, size);
  }
  return realloc(ptr, size);
}

void cai_free_mem(const cai_allocator *allocator, void *ptr) {
  if (ptr == NULL) {
    return;
  }
  if (allocator != NULL && allocator->free_fn != NULL) {
    allocator->free_fn(allocator->context, ptr);
    return;
  }
  free(ptr);
}

char *cai_strndup(const cai_allocator *allocator, const char *value,
                  size_t length) {
  char *copy;

  copy = (char *)cai_alloc(allocator, length + 1U);
  if (copy == NULL) {
    return NULL;
  }
  if (length > 0U) {
    memcpy(copy, value, length);
  }
  copy[length] = '\0';
  return copy;
}

char *cai_strdup(const cai_allocator *allocator, const char *value) {
  if (value == NULL) {
    return NULL;
  }
  return cai_strndup(allocator, value, strlen(value));
}
