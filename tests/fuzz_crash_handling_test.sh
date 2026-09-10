#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 2 || ! -d "$2" ]]; then
  printf 'usage: %s <source-root> <existing-build-dir>\n' "$0" >&2
  exit 2
fi
repo_root=$1
build_dir=$2
descriptor=$("$repo_root/scripts/cpkt-aflpp.sh" discover)
cc=$(sed -n 's/^cc=//p' <<<"$descriptor")
afl=$(sed -n 's/^afl_fuzz=//p' <<<"$descriptor")
showmap=$(sed -n 's/^afl_showmap=//p' <<<"$descriptor")
test_root=$(mktemp -d "$build_dir/crash-contract.XXXXXX")
trap 'rm -rf "$test_root"' EXIT
mkdir -p "$test_root/scripts" "$test_root/build/fuzz" "$test_root/tests/fuzz-corpus/tool"
cp "$repo_root/scripts/fuzz.sh" "$test_root/scripts/"
printf '#!/usr/bin/env bash\nprintf "afl_fuzz=%%s\\n" %q\n' "$afl" >"$test_root/scripts/cpkt-aflpp.sh"
chmod +x "$test_root/scripts/cpkt-aflpp.sh"
cat >"$test_root/fixture.c" <<'EOF'
#include <signal.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <unistd.h>
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
  if (prctl(PR_GET_DUMPABLE, 0L, 0L, 0L, 0L) != 0) _exit(99);
  if (size && data[0] == 'C') raise(SIGSEGV);
  if (size && data[0] == 'A') raise(SIGABRT);
  if (size && data[0] == 'H') for (;;) pause();
  return 0;
}
EOF
binary="$test_root/build/fuzz/cai_tool_fuzz"
"$cc" -Wall -Wextra -Werror "$repo_root/tests/fuzz_driver.c" "$test_root/fixture.c" -o "$binary"
[[ "$("$binary" --check-fuzz-isolation)" == cai-fuzz-no-core-v1 ]]
for case in N:0 C:2 A:2 H:1; do
  printf '%s' "${case%:*}" >"$test_root/input"
  status=0
  AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 "$showmap" -q -t 1000 \
    -o "$test_root/trace" -- "$binary" "$test_root/input" >"$test_root/showmap.log" 2>&1 || status=$?
  if [[ "$status" != "${case#*:}" ]]; then
    cat "$test_root/showmap.log" >&2
    printf 'Expected %s, got %s for %s\n' "${case#*:}" "$status" "${case%:*}" >&2
    exit 1
  fi
done
cat >"$test_root/mutator.c" <<'EOF'
#include <stddef.h>
void *afl_custom_init(void *afl, unsigned int seed) {
  (void)afl; (void)seed; return (void *)1;
}
size_t afl_custom_fuzz(void *data, unsigned char *buf, size_t size,
                      unsigned char **out, unsigned char *add,
                      size_t add_size, size_t max_size) {
  static unsigned char crash = 'C';
  (void)data; (void)buf; (void)size; (void)add; (void)add_size; (void)max_size;
  *out = &crash; return 1;
}
void afl_custom_deinit(void *data) { (void)data; }
EOF
"$cc" -Wall -Wextra -Werror -shared -fPIC "$test_root/mutator.c" -o "$test_root/mutator.so"
printf N >"$test_root/tests/fuzz-corpus/tool/normal"
status=0
AFL_CUSTOM_MUTATOR_LIBRARY="$test_root/mutator.so" AFL_CUSTOM_MUTATOR_ONLY=1 \
  CAI_FUZZ_SECONDS=2 bash "$test_root/scripts/fuzz.sh" smoke >"$test_root/runner.log" 2>&1 || status=$?
[[ "$status" != 0 ]]
grep -q 'Fuzz failure retained for reproduction:' "$test_root/runner.log" || { cat "$test_root/runner.log" >&2; exit 1; }
crash=$(find "$test_root/build/fuzz/afl" -path '*/crashes/id:*' -type f -print -quit)
printf C >"$test_root/expected"
cmp "$test_root/expected" "$crash"
printf 'AFL crash handling: normal, SIGSEGV, SIGABRT, hang, and retained crash gate passed\n'
