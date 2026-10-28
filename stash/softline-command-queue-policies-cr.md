# Change request: native queue policies for agent commands

Status: implemented upstream in Softline 0.14.0 and integrated into libcai_app.
CAI uses `sl_set_prompt_queue_policy`, `sl_prompt_queue_set_executable` and
native `eligible`/`stopped` inspection. The requirements and 0.13.0 diagnosis
below are retained as historical design context; no dependency patch is used.
Reference: Softline v0.13.0 public C API, implementation and `examples/chat.c`.
Relevant upstream sources: [queue implementation](https://github.com/sa6mwa/softline/blob/v0.13.0/src/softline.c)
and [chat example](https://github.com/sa6mwa/softline/blob/v0.13.0/examples/chat.c).

## Outcome

An agent application should be able to route **every typed slash command and
local shell command through Softline's visible, editable queue**, whether the
agent is busy or idle. Softline owns identity, editing, approval, cancellation,
history and queue presentation. The host only classifies input and declares
which execution boundary is ready.

There must be no second application queue and no fake empty submissions,
profile resets or dequeue/reinsert cycles to influence native delivery.

## Current gap

The queued-turns profile correctly queues busy input but submits idle drafts
directly. `sl_prompt_queue_enqueue_draft()` can queue an idle command, but is a
passive enqueue: it neither synchronizes automatic delivery nor resumes the
stopped FIFO after Escape. `sl_set_prompt_queue_delivery(AUTO)` synchronizes
delivery but deliberately preserves that stopped state.

Only a direct submission or Alt-Enter promotion clears the native stopped flag
(`sl_prompt_queue_resume_on_user_turn`). There is no public equivalent for an
explicit user action which is admitted through the queue instead.

Consequently, simply always calling enqueue_draft breaks this sequence:

1. Start a goal.
2. Escape cancels the turn and pauses the goal.
3. Type `/goal resume` and press Enter.
4. The new command remains stopped instead of executing and resuming pursuit.

Treating commands as special app-owned pending work would conceal them from
Alt-E. Resetting Softline's profile would lose cancellation semantics. Neither
is an acceptable application workaround.

## Required policies

### Admission

Keep the existing profiles unchanged. Add an opt-in way to enqueue an explicit
user submission even while idle. A classification callback or a submission
method accepting policy flags are possible API shapes; the behavior matters
more than the spelling.

- Ordinary chat prompts can retain today's busy/idle semantics.
- Typed `/COMMAND` and `!COMMAND` can always enter the native queue.
- Passive enqueue (Tab, background injection) remains distinct from deliberate
  Enter/Alt-Enter submission.
- Return or expose the affected identity. Editing must preserve that identity,
  approval state and original position, including on a full queue.
- Record prompt history once. A failed admission preserves the draft.
- Search selection, paste, empty edits and suspended editor restoration keep
  their existing behavior.

### Delivery eligibility

Support an execution hold independent of human approval. The host must be able
to mark an entry not yet executable without consuming it, altering its intent,
or displaying an approval request. Changing eligibility should schedule native
delivery when appropriate, without polling or simulated input.

The host has three useful classes; these are application semantics, not
special command names which Softline should recognize:

| Class | Example | Host eligibility |
| --- | --- | --- |
| Feed-local | `/status`, `/goal`, `/output full` | Between feed writes; no agent mutation |
| Agent seam | `/model MODEL`, `/goal pause` | Ordinary Enter waits for the next turn; Alt-Enter can use the next safe model/tool seam |
| Whole-turn | `/review`, `/new`, `/trim`, `/compact`, `/shell` | Whole turn complete, relevant tools/subagents/shell idle |

Approval, editing and execution holds all prevent delivery. A steering intent
does not waive a whole-turn requirement. Empty-editor Alt-Enter must obey the
same eligibility as Alt-Enter with a draft. The host must never have to pop an
entry to discover that it is ineligible.

Feed-local queries should remain eligible behind an unrelated blocked command,
without reordering, consuming or silently approving that command. Ordinary user
turns retain FIFO ordering. Expose enough entry state for an owner-thread host
consumer to obey the same policy as automatic next_prompt delivery.

### Escape and explicit resumption

Expose stopped-delivery state and define resumption independently of whether
next_prompt happened to return DIRECT, QUEUED or PROMOTED.

- Escape retains queued work and pauses automatic release.
- Passive enqueue and background injection never undo that pause.
- A new read-only command such as `/status` may execute but must not release
  old queued user prompts or goal controls.
- A fresh deliberate action such as `/goal resume` must be executable without
  requiring a second Alt-Enter, despite being admitted through the queue.
- Already stopped entries remain editable and must not become eligible merely
  because a status query was issued. Define explicitly whether an action also
  resumes the old FIFO; do not conflate executing that action with resumption.
- Editing an old stopped entry is not a fresh submission unless the user
  explicitly promotes/submits it under the chosen policy.

The preferred policy is that fresh explicit commands can execute once their
host boundary is ready, while pre-existing stopped entries stay paused until
the user explicitly resumes that backlog. Softline should own that distinction
instead of requiring the host to maintain its own cancellation generations.
Provide an explicit resume-backlog operation as well, for hosts which want
today's new-user-turn behavior. Do not make a global AUTO setter clear a stop.

### Threading and execution

All queue/editor methods remain owner-thread operations. Readiness changes can
arrive through Softline's existing watch/wakeup integration. A host callback
must be short; Softline does not execute tools, fetch catalogs, open databases,
or run subprocesses. Admission and asynchronous execution are separate concerns.
Native terminal handoff shortcuts such as Alt-S may remain distinct from typed
commands, preserving their draft/undo continuation.

### C and Lua parity

Expose the same policy, entry-state inspection, eligibility and explicit
resumption operations through both receiver APIs. Avoid a C-only policy which
requires Lua hosts to reconstruct queue behavior.

## Acceptance tests

Use event-gated terminal tests, with no inference or arbitrary sleeps:

1. Idle Enter queues a command; Alt-E in the same input burst retrieves it
   before delivery. Editing replaces the same identity; the original never runs.
2. Busy Enter and Alt-Enter retain their ordinary/steering intentions.
3. A whole-turn command remains editable through multiple tool seams and runs
   once at the eligible boundary, including after empty-editor promotion.
4. A feed-local query runs promptly behind a blocked command without consuming it.
5. Escape retains the backlog. Passive injection and `/status` do not release it.
6. A fresh `/goal resume` Enter executes after Escape without losing its queued
   identity or automatically executing previously stopped work.
7. Queue edits, rejection, full capacity and held incoming messages preserve
   identity, ordering, approval and draft text under every admission policy.
8. While a local shell runs, a normal prompt remains in the native queue and
   can be edited; ending the shell releases the appropriate next entry.
9. C and Lua produce equivalent behavior; normal existing profiles still pass
   their current queue, history, approval and terminal handoff regressions.

## CAI cutover boundary

CAI can independently remove forced busy `!COMMAND` submission, account for
shell activity in its busy signal, retain idle-only commands at tool seams,
and classify display preferences as feed-local. Uniform **idle** command
admission remains deferred until explicit queued-submission/cancellation policy
exists upstream. CAI must not ship a workaround that strands `/goal resume`
after Escape.
