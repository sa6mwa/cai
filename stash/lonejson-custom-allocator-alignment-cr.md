# lonejson CR: document and guard custom allocator alignment

## Summary

cai hit an ASAN/UBSAN failure while parsing a normal Responses JSON fixture with
lonejson 0.5.0 under the `asan` preset:

```text
.cache/deps/lonejson-0.5.0/include/lonejson.h:3582:26:
runtime error: member access within misaligned address ... for type
'union lonejson__alloc_header', which requires 16 byte alignment

SUMMARY: AddressSanitizer: SEGV ... in lonejson__owned_malloc
```

The call path was:

```text
cai_response_parse_json
  lonejson_parse_cstr
    lonejson__assign_string
      lonejson__owned_malloc
```

## Root Cause

cai passed a custom parse allocator to lonejson. That allocator prepended a
private `size_t` header and returned `header + 1` to callers. On x86_64 this
made the returned pointer 8-byte aligned, not 16-byte aligned.

lonejson then used that custom allocator result as storage for its internal
`lonejson__alloc_header`, which contains a `long double` alignment member and
requires 16-byte alignment on this platform. Standard `malloc` satisfies that,
but cai's custom allocator did not.

So the immediate cai bug was an invalid allocator implementation: allocator
callbacks need to behave like `malloc`/`realloc`/`free`, including returning
storage suitably aligned for any C object type.

## Requested lonejson Change

Please make the allocator alignment contract explicit in the public docs near
`lonejson_allocator`:

- `malloc_fn` and `realloc_fn` must return pointers aligned at least as strictly
  as standard `malloc`.
- Returning a pointer with weaker alignment is undefined behavior because
  lonejson may place internal owned-allocation headers at that address.
- Custom allocator examples that add private headers must over-align their
  returned payload pointer, for example by using a union/header that includes
  `void *`, integer, `double`, and `long double` alignment members.

If reasonable for debug builds, add an internal alignment assertion/check before
casting custom allocator results to `lonejson__alloc_header *`. A runtime check
cannot repair a bad allocator, but it would turn this into an actionable
`INVALID_ARGUMENT`/internal diagnostic instead of an opaque sanitizer crash.

## cai-side Fix

cai changed its zeroing allocator header from:

```c
typedef struct cai_zero_alloc_header {
  size_t size;
} cai_zero_alloc_header;
```

to an over-aligned union:

```c
typedef union cai_zero_alloc_header {
  struct {
    size_t size;
  } meta;
  void *align_ptr;
  long align_long;
  long long align_long_long;
  double align_double;
  long double align_long_double;
} cai_zero_alloc_header;
```

That makes the payload returned by `header + 1` suitable for lonejson's owned
allocation wrapper on the tested platform.
