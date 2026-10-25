# Conversation messaging

CAI and `libcai_app` enable messaging only when explicitly configured with
`--messaging`, `CAI_MESSAGING=true`, or `[messaging] enabled = true`. The
`agent_message` tool and queue listeners are disabled by default. Workspaces
using the same encrypted Pouch root can exchange messages, including across
processes and working directories. Core `libcai` has no liblockdc or softline
dependency.

## Sending and delivery

For a standalone send, use `cai --queue NAME --message TEXT` or
`cai -q NAME -M TEXT`. (`-m` continues to select a model.) This utility uses
the selected `storage.messaging_store` (`--messaging-store` or
`CAI_MESSAGING_STORE`), without provider authentication or inference. The default
is the `default` named store configured by `--lockd`; it can instead use a
separate `[store.NAME] endpoint` and `key_file`. Temporary conversations retain
this shared messaging selection while isolating their conversation history.
It creates no conversation, mailbox claim or receipt listener. The sender is
`cai.cli.<unique-id>`; incoming text is labelled as a CLI message.
Success means liblockdc accepted the enqueue: stdout contains JSON with
`status: "queued"`, the message ID and recipient, and the command exits zero.
It does not wait for delivery, approval or execution. Offline recipients are
supported; these messages have no delivery deadline or queue expiry.
Invalid input or storage errors use pslog on stderr and return nonzero.
`--message-ack-timeout-ms` applies to the agent tool, not standalone sends.
Explicit sending works even when the agent messaging tool is disabled. The same
flags work through the C or Lua `libcai_app` receiver's `run` method.

Use `agent_message` with `{"operation":"list"}` to list named workspace
mailboxes: queue name, canonical directory and last-bound conversation ID.
Send with `{"operation":"send","recipient":"QUEUE_NAME","text":"..."}`.
Conversation IDs are internal history/ownership metadata, not destinations.
Discovery lists known mailboxes, including offline ones; delivery ACK confirms
acceptance. An idle application listens too.

The first workspace with a given basename normally receives `cai.<basename>`.
When that name is occupied, CAI selects the fewest real ancestor components
needed to distinguish the workspace from registered workspaces with that
basename. Components remain in path order but need not be adjacent. Equally
small choices prefer ancestors nearest the basename; character length has no
weight. No hashes, invented qualifiers, abbreviations or shortened names are
generated.

For example, if `/lab` already owns `cai.lab`, `/workspaces/mike/g/lab` receives
`cai.g.lab`. If `/workspaces/alice/g/lab` owns `cai.lab`, `/workspaces/mike/g/lab` receives
`cai.mike.lab`: the shared `g` provides no distinction. Existing names stay
unchanged, so allocation depends on registration order.

The actual hostname is one leading component before the canonical directory
path. It is normally shared and omitted; identical paths on different hosts
can be distinguished by the hostname. Workspace identity uses the Linux
machine-id together with the canonical path. New allocations for identical
hostname/path pairs on distinct hosts require `--message-queue NAME`; established
assignments remain usable. A missing
machine-id also requires an explicit name. Symlink aliases resolve to the same
workspace. Assigned names persist as mailbox state; allocation and claims are
serialized by a shared naming lease, including concurrent application starts.

Every automatic name preserves the selected components exactly. The backend
only accepts lowercase ASCII letters, digits, `.`, `_` and `-`, with at most
128 bytes per queue name. Unsupported basenames, names that cannot fit unchanged,
or paths with no distinguishing component selection require an explicit override.

`/status` shows the active queue name and whether incoming prompts require
manual approval or are automatically approved. `/new` includes the active queue
in its Markdown session summary when messaging is enabled.
Override it with `--message-queue NAME`,
`CAI_MESSAGE_QUEUE`, or `[messaging] queue_name`. Empty keeps automatic naming.
Overrides replace the entire name; no prefix or path hash is appended.
Explicit names are lowercased and must contain only ASCII letters, digits,
`.`, `_` or `-`, with at most 128 characters. Deliberately using the same
explicit name in another directory takes over that mailbox too.

In the TUI, `/queue` shows the current mailbox (or how to enable it),
`/queue on` enables messaging and its tool, and `/queue off` disables them.
Enabling and disabling apply between turns, retaining the same conversation
and saved model settings without replaying its text or making a model request.
Approve or reject queued incoming prompts before disabling messaging; this
protects any edits made in the prompt queue. Deliveries still in storage remain
available when messaging is enabled again. `/autoqueue` remains the separate
automatic-approval setting and is retained across these changes and `/new`.
`/queue NAME` replaces the full name between turns, applying the same takeover
rules. For example,
`/queue Smith` selects `smith`. The override lasts for the running app,
including `/new` and `/resume`; these commands do not rewrite `config.ini` or
session settings. Delivered messages and their approvals remain in the conversation.
The former destination stays listed as a known mailbox but no longer has this
app as its active listener. If starting the new listener fails, the error names
the selected queue; retry with `/queue NAME`.

## Ownership and takeover

A new app start claims the workspace mailbox for its active conversation,
including after `/new` or resuming a different conversation. The address stays
the same. Binding updates and durable message acceptance use the same lockd
lease, so a superseded process cannot accept new deliveries into its old
conversation. A stale consumer defers the message without consuming its retry
budget and stops its mailbox service. It reports the ownership change through
the normal logger. No application polling is used.

Already delivered messages stay with their original conversation, including
held approval entries. They can still be approved there or recovered by resuming
that conversation. They are never transferred just because another app starts.
The old conversation can keep running, but new sends through its displaced
mailbox fail with an ownership error. Restart it to reclaim the mailbox, or
choose a different name. Exiting the displaced app cannot clear the new binding;
exiting the new owner does not automatically reactivate an older app.

Each listener uses liblockdc's managed consumer services. Filesystem queue
notifications wake that service; the application does not periodically query
the queue. The service saves an incoming message to a durable encrypted inbox
before sending a delivery receipt when the sender requested one. An event
descriptor then wakes the UI owner.
Recovery queries use an indexed `pending_for` field scoped to the conversation
that accepted the message. Bindings and pending records live in `cai.messages`.
Private `cai.receipts.<instance-id>` channels deliver ACKs to the exact sending
app instance, independently of mailbox takeover. They are internal return
channels and are excluded from destination discovery.

The agent tool's `send` waits for that receipt, with a five-minute deadline measured from sending.
An offline recipient or absent receipt returns a tool error. Expired messages
that have not been accepted cannot later start turns. Approval and model
execution are independent of ACK: a held or subsequently rejected message is
still delivered. Cancellation interrupts the sender's wait; it does not recall
an already delivered message.

## Approval and editing

Incoming text is labelled with its message ID and sending conversation. It uses
softline 0.12's **normal prompt injection hook**, never the steering hook.

| Key or command | Action |
| --- | --- |
| Alt+A | Focus the oldest pending approval |
| Alt+Y | Approve the focused message |
| Alt+N | Reject the focused message |
| Alt+E | Edit and requeue, preserving message identity and approval |
| `/autoqueue` | Toggle automatic approval |
| `/autoqueue on` | Approve waiting messages and automatically approve future arrivals |
| `/autoqueue off` | Require approval for future arrivals; existing approvals remain valid |

An approved message remains in softline's queue until the running turn and
shell activity finish. It then starts an ordinary next turn, before automatic
goal continuation or the next startup instruction;
it is not injected between tool calls. A message can be edited while waiting.
Editor changes live in softline until submission; reopening an unsubmitted
message restores the original delivered text.
Unapproved messages survive restart in the inbox. Rejected and consumed IDs are
retained without message bodies to suppress repeated queue delivery.

Non-interactive agents can send messages and acknowledge incoming delivery.
Incoming prompts remain pending for a subsequent interactive run; automatic
approval is a TUI setting. A batch invocation still exits when its requested
instructions or goal finish, rather than becoming a message daemon.

## Configuration

The generated `config.ini` contains these commented defaults:

```ini
[messaging]
## Shared conversation messaging.
# enabled = false
# queue_name = ""
# enable_auto_enqueue = false
# ack_timeout_ms = 300000
```

| INI key | Environment | Command line |
| --- | --- | --- |
| `messaging.enabled` | `CAI_MESSAGING` | `--messaging` / `--no-messaging` |
| `messaging.queue_name` | `CAI_MESSAGE_QUEUE` | `--message-queue NAME` |
| `messaging.enable_auto_enqueue` | `CAI_ENABLE_AUTO_ENQUEUE` | `--autoqueue` / `--no-autoqueue` |
| `messaging.ack_timeout_ms` | `CAI_MESSAGE_ACK_TIMEOUT_MS` | `--message-ack-timeout-ms MS` |

CLI overrides environment, which overrides INI defaults, with the existing
`[env]` precedence rule. The deadline range is 1–86400000 ms. `/autoqueue`
changes the live TUI setting; it does not rewrite configuration.
`libcai_app` C settings and Lua `settings.messaging` accept these same keys.
Isolated reviews do not register the messaging tool or start a listener.

Text bodies are limited to 64 KiB; newline and tab are allowed, other ASCII
control characters are rejected. Parsing and storage use bounded buffers and streaming
JSON readers/producers. Normal activity uses the application's pslog sink,
including `agent_message_received` at info level with `role=agent`, message ID
and sender session ID.

## Core integration boundary

Other hosts may register transport tools through
`cai_agent_runtime_config.register_agent_tools`, called on the owner thread
during runtime open. It receives a borrowed agent and an application context.
Tool contexts must outlive the runtime; handlers run on native workers. Callback
failure aborts open. Child agents do not inherit the callback; isolated review
rejects it.

For incoming approved messages, C hosts use `submit_queued` on the owner thread
or `submit_queued_threadsafe` from another thread. Lua hosts use
`runtime:submit_queued(text)` on their owning Lua thread. These next-turn APIs
also work with transports unrelated to LQ. The native registration callback is
a C interface, not a Lua callback running on a worker.

## Scope and recovery limits

`cai_app_messages_roundtrip` runs a C embedder linked to the static application
library alongside a Lua embedder using the shared application library. Both
stay running against the same explicit encrypted Pouch root. It verifies
destination discovery, a message in each direction, delivery ACKs before
approval, and both approved messages reaching the receiving model context.
The provider responses come from a local HTTP fixture; transport, storage,
tools, approval and application instances use the production implementation.
The same test starts another app on the original directory, confirms the same
queue name routes new deliveries there, approves an older held message in the
original conversation, and verifies that closing the displaced app leaves the
new owner's binding intact. Native tests additionally exercise symlink naming,
equal basenames, explicit names, collision refusal and stale-consumer deferral.

The tested backend is shared local Pouch. Remote lockd fanout and the live
kanban HTTP page remain future work. Use different directories or explicit queue
names for independently addressable agents. Delivery is durable and queue redelivery is deduplicated; execution
is not claimed to be exactly once. A crash between persisting a runtime turn
and marking its inbox entry consumed may present that message again on resume.
