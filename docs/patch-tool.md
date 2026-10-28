# Native patch tool

CAI implements the Codex `apply_patch` format in C89. No external patch program,
Rust runtime or VCS comparison is needed. Direct `cai_apply_patch` and registered
freeform tool calls share the matcher; this also serves Smith and libcai_app.

## Matching

Hunks are matched against the original file in source order. Each search starts
after the preceding hunk's matched lines. An `@@ context` header first advances
past that context line. A context-only hunk also advances the cursor. Repeated
text selects the first matching occurrence after that cursor; uniqueness across
the file is not required.

For each hunk, search all candidates at each precedence before relaxing it:

1. Exact logical line contents.
2. Ignore trailing Unicode whitespace.
3. Ignore leading and trailing Unicode whitespace.
4. Trim edges and normalize Codex's curly quote, dash/minus and unusual-space
   characters to their ASCII counterparts.

Thus a later exact match wins over an earlier whitespace-only match. Arbitrary
internal spacing and unrelated text are not normalized. Patterns match whole
lines, never suffixes within another line. `*** End of File` targets the final
source lines. A trailing empty context line can be omitted when it represents
the file's terminating newline, matching Codex's fallback.

An insertion-only update hunk appends at EOF. Updates can begin without `@@`,
and a bare empty line within a hunk represents empty context. Empty hunks and
malformed operations fail. Later hunks cannot match text inserted by earlier
hunks because matching uses the original source.

Unchanged lines retain their actual bytes and line endings, including when a
fallback matched different context spelling. Inserted lines use the original
file's first line-ending style, or LF for a file without one. Updates terminate
the resulting final line, including an originally unterminated line. LF, CRLF
and CR source endings are recognized; mixed unchanged endings are preserved.

The line index and replacement plan are bounded by configured file/patch limits.
The result is built once in source order; each append checks the result byte
limit before growing the buffer. This API materializes bounded patch/file data;
it does not claim to stream file edits.

## Validation and failures

Codex-compatible matching does not waive CAI's workspace path restrictions,
symlink/hardlink rules, source identity checks or byte limits. All file changes
are validated before publication. A missing hunk, invalid path, excessive result
or other preflight failure leaves every file untouched. The existing publication
and rollback machinery handles filesystem failures.

Failures identify the file and hunk and provide bounded escaped context where
available. Read that location before retrying. The interactive feed shows the
ordered tool failure; attributed duplicate warnings remain in the log instead
of repeating it in the feed. These semantics do not rewrite old transcript text.

## Evidence

The compatibility reference is Codex's `seek_sequence.rs`, `file_update.rs`,
`text_file.rs` and `streaming_parser.rs` at source revision `0c329ad2c9`.
Local comparison against the installed Codex executable also covers the actual
three rejected profile patches from session `dbgtfervs68i52ohr8gg`. Normal tests
use deterministic expectations and do not require a Codex installation.

`cai_tests.patch_codex_matching` exercises both direct and spooled tool paths.
The broader patch group retains path, limit, identity, rollback and atomicity
checks. CLI tests use an offline provider to verify actual tool calls, receipts,
context/line positions, failures and duplicate-warning suppression.
