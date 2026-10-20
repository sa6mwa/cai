# Conversation messaging

CAI and `libcai_app` expose the `agent_message` tool by default. Conversations
using the same encrypted Pouch root can exchange messages, including across
processes and working directories. Core `libcai` has no liblockdc or softline
dependency.

## Sending and delivery

Use `agent_message` with `{"operation":"list"}` to list stored conversations.
Then use `{"operation":"send","recipient":"CONVERSATION_ID","text":"..."}`.
The recipient must be running with messaging enabled to acknowledge delivery.
An idle conversation listens too. Addresses are `cai.session.<conversation-id>`
in the `cai.messages` namespace; `/new` gets a different address.

Each listener runs liblockdc's managed consumer service. Filesystem queue
notifications wake that service; the application does not periodically query
the queue. The service saves an incoming message to a durable encrypted inbox
before sending a delivery receipt. An event descriptor then wakes the UI owner.
Recovery queries use an indexed `pending_for` field, scoped to the recipient.

`send` waits for that receipt, with a five-minute deadline measured from sending.
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
# enabled = true
# enable_auto_enqueue = false
# ack_timeout_ms = 300000
```

| INI key | Environment | Command line |
| --- | --- | --- |
| `messaging.enabled` | `CAI_MESSAGING` | `--messaging` / `--no-messaging` |
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

The tested backend is shared local Pouch. Remote lockd fanout and the live
kanban HTTP page remain future work. Use a distinct active conversation for
each agent. Delivery is durable and queue redelivery is deduplicated; execution
is not claimed to be exactly once. A crash between persisting a runtime turn
and marking its inbox entry consumed may present that message again on resume.
