#!/usr/bin/env bash
source "$(dirname "$0")/cli_test_common.sh"
protocol=${1:-responses}
peer_start
config="$root/config/cai/config.ini"
mkdir -p "$(dirname "$config")" "$root/.codex" "$root/custom-codex" "$work/.git" "$work/nested"
printf 'CAI_GLOBAL_MARKER\n' >"$root/config/cai/AGENTS.md"
printf 'CODEX_DEFAULT_MARKER\n' >"$root/.codex/AGENTS.md"
# Exercise complete multi-chunk user context, not merely a short marker.
for ((i=0;i<180;i++)); do
  printf 'User context row %03d: literal "quotes", backslashes \\ and Unicode café.\n' "$i"
done >>"$root/.codex/AGENTS.md"
printf 'CODEX_CUSTOM_MARKER\n' >"$root/custom-codex/AGENTS.md"
printf 'ANCESTOR_MARKER\n' >"$work/AGENTS.md"
printf 'WORKSPACE_MARKER\n' >"$work/nested/AGENTS.md"
unset CODEX_HOME
base=("$cli" -C "$work/nested")
write_config() {
  cat >"$config" <<INI
[client]
provider = custom
base_url = "$endpoint"
api_protocol = $protocol
[agent]
model = gpt-6-luna
image_generation = false
terminal = false
codex_agents_md = $1
INI
}
turn() {
  plan "$(jq -n --argjson chat "$([[ "$protocol" == chat_completions ]] && printf true || printf false)" '{chat:$chat,plan:["AGENTS TURN DONE"]}')"
  run "$@"; ok
  tail -1 "$root/peer/requests.jsonl" >"$root/request"
  jq -r '.instructions // (.messages[] | select(.role == "system" or .role == "developer") | .content)' "$root/request" >"$root/instructions"
  contains "$root/instructions" CAI_GLOBAL_MARKER
  contains "$root/instructions" WORKSPACE_MARKER
}
write_config false
turn -Nni 'before enabling Codex instructions'
absent "$root/instructions" CODEX_DEFAULT_MARKER
absent "$root/instructions" ANCESTOR_MARKER
# Existing encrypted session; enable global instructions without --new.
write_config true
turn -ni 'resume with Codex instructions'
contains "$root/request" 'before enabling Codex instructions'
contains "$root/instructions" CODEX_DEFAULT_MARKER
contains "$root/instructions" '# AGENTS.md instructions from'
contains "$root/instructions" "current user's instructions and working context"
contains "$root/instructions" '<INSTRUCTIONS>'
contains "$root/instructions" '</INSTRUCTIONS>'
jq -e --rawfile expected "$root/.codex/AGENTS.md" \
  '.instructions // ([.messages[] | select(.role == "system" or .role == "developer") | .content] | join("\n")) | contains($expected)' \
  "$root/request" >/dev/null
contains "$root/instructions" ANCESTOR_MARKER
awk '/CAI_GLOBAL_MARKER/{c=NR} /CODEX_DEFAULT_MARKER/{d=NR} /ANCESTOR_MARKER/{a=NR} /WORKSPACE_MARKER/{w=NR} END{exit !(c<d && d<a && a<w)}' "$root/instructions"
# File edits and CODEX_HOME are resolved again on the next startup/replay.
printf 'CODEX_UPDATED_MARKER\n' >"$root/.codex/AGENTS.md"
turn -ni 'resume after editing global instructions'
contains "$root/instructions" CODEX_UPDATED_MARKER
absent "$root/instructions" CODEX_DEFAULT_MARKER
export CODEX_HOME="$root/custom-codex"
turn -ni 'resume with custom Codex home'
contains "$root/instructions" CODEX_CUSTOM_MARKER
absent "$root/instructions" CODEX_UPDATED_MARKER
# Disabling compatibility must also take effect on that same conversation.
write_config false
turn -ni 'resume without Codex compatibility'
absent "$root/instructions" CODEX_CUSTOM_MARKER
absent "$root/instructions" ANCESTOR_MARKER
turn --codex-agents-md -ni 'explicit compatibility flag'
contains "$root/instructions" CODEX_CUSTOM_MARKER
(
  export CAI_CODEX_AGENTS_MD=true
  turn -ni 'environment enables current global instructions'
  contains "$root/instructions" CODEX_CUSTOM_MARKER
  turn --no-codex-agents-md -ni 'explicit opt out overrides the environment'
  absent "$root/instructions" CODEX_CUSTOM_MARKER
)
# Config-driven environment and an explicit model change must both preserve
# fresh global instructions when the saved session is loaded again.
unset CODEX_HOME
write_config true
printf '[env]\nCODEX_HOME = "%s"\n' "$root/custom-codex" >>"$config"
turn -ni 'change model while retaining global instructions' -m gpt-6-astra
contains "$root/instructions" CODEX_CUSTOM_MARKER
turn -ni 'restore the saved model and current instructions'
json "$root/request" '.model == "gpt-6-astra"'
contains "$root/instructions" CODEX_CUSTOM_MARKER
# Every startup records resolved paths, rather than relying on a model's claim
# that it saw an instruction. No configuration comes from the conversation.
contains "$root/stderr" 'Global AGENTS.md loaded'
contains "$root/stderr" "$root/custom-codex/AGENTS.md"
# The interactive /resume path reopens the runtime with current startup
# settings and freshly reads instruction files. Editing config.ini alone is
# deliberately not a live configuration reload.
plan "$(jq -n --argjson chat "$([[ "$protocol" == chat_completions ]] && printf true || printf false)" '{chat:$chat,plan:["InteractiveAgentsDone"]}')"
terminal_start
printf 'CODEX_INTERACTIVE_MARKER\n' >"$root/custom-codex/AGENTS.md"
write_config false
send '/resume\r'; expect 'resume'
send '/resume 1\r'; expect 'restore the saved model and current instructions AGENTS TURN DONE'
prompt 'check instructions after manual resume' InteractiveAgentsDone
tail -1 "$root/peer/requests.jsonl" >"$root/request"
contains "$root/request" CODEX_INTERACTIVE_MARKER
send '/status\r'; expect 'Status'
expect 'cai global AGENTS.md'; expect 'Codex global AGENTS.md'
expect 'Repository AGENTS.md'
has "$term/text" 'Yes' || fail 'Captured global instruction sources not reported'
terminal_close
