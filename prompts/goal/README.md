# Goal continuation

`codex-continuation.md` is the upstream continuation prompt from OpenAI Codex
commit `0c329ad2c9`, `codex-rs/ext/goal/templates/goals/continuation.md`
(Apache-2.0). Keep it intact as the comparison baseline.

`continuation.md` is the embedded CAI template. It preserves the continuation,
objective, budget, evidence, fidelity, completion and blocked audits. CAI omits
the unavailable `update_plan` guidance and permits the model to pause for user
input or approval, then resume a paused goal when ready. Completed goals cannot
be resumed. Templates are compiled into libcai; runtime files are not required.
