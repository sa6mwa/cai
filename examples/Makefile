SHELL := bash
.DEFAULT_GOAL := help
MAKEFLAGS += --no-print-directory
.PHONY: help lua-rock lua-env run-basic-response run-chatgpt-login run-openrouter-response run-conversation-handles run-streaming-text run-history-export run-session-state run-smhi-weather run-mcp-server run-searxng-search run-terminal-chat run-mike-mind run-lua-chatgpt-login run-lua-basic run-lua-terminal-chat run-lua-conversation run-lua-session-state

ROOT := ..
CMAKE ?= cmake
LUA ?= lua

CAI_SESSION_STATE_PATH ?= /tmp/cai-session-state.json
CAI_LUA_SESSION_STATE_PATH ?= /tmp/cai-lua-state.json
CAI_SMHI_LOCATION ?= Gothenburg
CAI_SEARXNG_QUERY ?= OpenAI Responses API
CAI_MCP_EXAMPLE_PORT ?= 18766
CAI_EXEC_TOOL_DIR ?= $(abspath $(ROOT))
CAI_READ_TOOL_DIR ?= $(abspath $(ROOT))
CAI_CHATGPT_AUTH ?=
CAI_CHATGPT_AUTH_JSON ?=
CAI_CHATGPT_LOGIN_PORT ?= 1455
CAI_CHATGPT_BROWSER_COMMAND ?=
CAI_TERMINAL_CHAT_MODEL ?=
CAI_TERMINAL_CHAT_MAX_INPUT_TOKENS ?=
CAI_TERMINAL_CHAT_MAX_CACHED_INPUT_TOKENS ?=
CAI_TERMINAL_CHAT_MAX_OUTPUT_TOKENS ?=
CAI_TERMINAL_CHAT_MAX_REASONING_OUTPUT_TOKENS ?=
CAI_TERMINAL_CHAT_MAX_TOTAL_TOKENS ?=
CAI_TERMINAL_CHAT_MAX_SPEND_USD ?=

ifneq ($(strip $(CAI_EXEC_TOOL_DIR)),)
EXEC_TOOL_ARGS := --exec-tool-dir "$(CAI_EXEC_TOOL_DIR)"
else
EXEC_TOOL_ARGS :=
endif
ifneq ($(strip $(CAI_READ_TOOL_DIR)),)
READ_TOOL_ARGS := --read-tool-dir "$(CAI_READ_TOOL_DIR)"
else
READ_TOOL_ARGS :=
endif
TERMINAL_TOOL_ARGS := $(EXEC_TOOL_ARGS) $(READ_TOOL_ARGS)
ifneq ($(strip $(CAI_TERMINAL_CHAT_MODEL)),)
TERMINAL_MODEL_ARGS := --model "$(CAI_TERMINAL_CHAT_MODEL)"
else
TERMINAL_MODEL_ARGS :=
endif
ifneq ($(strip $(CAI_CHATGPT_AUTH_JSON)),)
CHATGPT_AUTH_ARGS := --chatgpt-auth-json "$(CAI_CHATGPT_AUTH_JSON)"
CHATGPT_LOGIN_ARGS := --auth-json "$(CAI_CHATGPT_AUTH_JSON)" --port "$(CAI_CHATGPT_LOGIN_PORT)"
else ifneq ($(strip $(CAI_CHATGPT_AUTH)),)
CHATGPT_AUTH_ARGS := --chatgpt-auth
CHATGPT_LOGIN_ARGS := --port "$(CAI_CHATGPT_LOGIN_PORT)"
else
CHATGPT_AUTH_ARGS :=
CHATGPT_LOGIN_ARGS := --port "$(CAI_CHATGPT_LOGIN_PORT)"
endif
ifneq ($(strip $(CAI_CHATGPT_BROWSER_COMMAND)),)
CHATGPT_LOGIN_ARGS += --browser-command "$(CAI_CHATGPT_BROWSER_COMMAND)"
endif
ifneq ($(strip $(CAI_TERMINAL_CHAT_MAX_INPUT_TOKENS)),)
TERMINAL_LIMIT_ARGS += --max-input-tokens "$(CAI_TERMINAL_CHAT_MAX_INPUT_TOKENS)"
endif
ifneq ($(strip $(CAI_TERMINAL_CHAT_MAX_CACHED_INPUT_TOKENS)),)
TERMINAL_LIMIT_ARGS += --max-cached-input-tokens "$(CAI_TERMINAL_CHAT_MAX_CACHED_INPUT_TOKENS)"
endif
ifneq ($(strip $(CAI_TERMINAL_CHAT_MAX_OUTPUT_TOKENS)),)
TERMINAL_LIMIT_ARGS += --max-output-tokens "$(CAI_TERMINAL_CHAT_MAX_OUTPUT_TOKENS)"
endif
ifneq ($(strip $(CAI_TERMINAL_CHAT_MAX_REASONING_OUTPUT_TOKENS)),)
TERMINAL_LIMIT_ARGS += --max-reasoning-output-tokens "$(CAI_TERMINAL_CHAT_MAX_REASONING_OUTPUT_TOKENS)"
endif
ifneq ($(strip $(CAI_TERMINAL_CHAT_MAX_TOTAL_TOKENS)),)
TERMINAL_LIMIT_ARGS += --max-total-tokens "$(CAI_TERMINAL_CHAT_MAX_TOTAL_TOKENS)"
endif
ifneq ($(strip $(CAI_TERMINAL_CHAT_MAX_SPEND_USD)),)
TERMINAL_LIMIT_ARGS += --max-spend-usd "$(CAI_TERMINAL_CHAT_MAX_SPEND_USD)"
endif
TERMINAL_CHAT_ARGS := $(CHATGPT_AUTH_ARGS) $(TERMINAL_MODEL_ARGS) $(TERMINAL_TOOL_ARGS) $(TERMINAL_LIMIT_ARGS)

define run_c_example
	cd "$(ROOT)" && \
	$(CMAKE) --preset debug && \
	$(CMAKE) --build --preset debug --target $(1) && \
	./build/debug/$(1) $(2)
endef

define run_lua_example
	cd "$(ROOT)" && \
	$(MAKE) lua-rock && \
	eval "$$($(MAKE) -s lua-env)" && \
	$(LUA) $(1) $(2)
endef

help:
	@printf '%s\n' \
		'cai example runners:' \
		'  make run-basic-response' \
		'  make run-chatgpt-login [CAI_CHATGPT_AUTH_JSON=/tmp/cai-auth.json] [CAI_CHATGPT_BROWSER_COMMAND=xdg-open]' \
		'  make run-openrouter-response' \
		'  make run-conversation-handles' \
		'  make run-streaming-text' \
		'  make run-history-export' \
		'  make run-session-state CAI_SESSION_STATE_PATH=/tmp/cai-session-state.json' \
		'  make run-smhi-weather CAI_SMHI_LOCATION=Gothenburg' \
		'  make run-mcp-server CAI_MCP_EXAMPLE_PORT=18766' \
		'  make run-searxng-search CAI_SEARXNG_QUERY="OpenAI Responses API"' \
		'  make run-terminal-chat [CAI_CHATGPT_AUTH=1|CAI_CHATGPT_AUTH_JSON=/tmp/cai-auth.json] [CAI_TERMINAL_CHAT_MODEL=gpt-5.4-mini] [CAI_EXEC_TOOL_DIR=/tmp/root] [CAI_READ_TOOL_DIR=$$PWD] [CAI_TERMINAL_CHAT_MAX_OUTPUT_TOKENS=1000000]' \
		'    run-terminal-chat defaults CAI_EXEC_TOOL_DIR and CAI_READ_TOOL_DIR to the repo root; pass empty values to disable them.' \
		'  make run-mike-mind' \
		'  make run-lua-chatgpt-login [CAI_CHATGPT_AUTH_JSON=/tmp/cai-auth.json] [CAI_CHATGPT_BROWSER_COMMAND=xdg-open]' \
		'  make run-lua-basic' \
		'  make run-lua-terminal-chat [CAI_CHATGPT_AUTH=1|CAI_CHATGPT_AUTH_JSON=/tmp/cai-auth.json] [CAI_TERMINAL_CHAT_MODEL=gpt-5.4-mini] [CAI_EXEC_TOOL_DIR=/tmp/root] [CAI_READ_TOOL_DIR=$$PWD] [CAI_TERMINAL_CHAT_MAX_OUTPUT_TOKENS=1000000]' \
		'    run-lua-terminal-chat defaults CAI_EXEC_TOOL_DIR and CAI_READ_TOOL_DIR to the repo root; pass empty values to disable them.' \
		'  make run-lua-conversation' \
		'  make run-lua-session-state CAI_LUA_SESSION_STATE_PATH=/tmp/cai-lua-state.json' \
		'' \
		'Most examples call the real API. Provide OPENAI_API_KEY, OPENROUTER_API_KEY,' \
		'or a repo-local .env as appropriate.'

lua-rock:
	@cd "$(ROOT)" && $(MAKE) lua-rock

lua-env:
	@cd "$(ROOT)" && $(MAKE) -s lua-env

run-basic-response:
	$(call run_c_example,cai_example_basic_response,)

run-chatgpt-login:
	$(call run_c_example,cai_example_chatgpt_login,$(CHATGPT_LOGIN_ARGS))

run-openrouter-response:
	$(call run_c_example,cai_example_openrouter_response,)

run-conversation-handles:
	$(call run_c_example,cai_example_conversation_handles,)

run-streaming-text:
	$(call run_c_example,cai_example_streaming_text,)

run-history-export:
	$(call run_c_example,cai_example_history_export,)

run-session-state:
	$(call run_c_example,cai_example_session_state,"$(CAI_SESSION_STATE_PATH)")

run-smhi-weather:
	$(call run_c_example,cai_example_smhi_weather,"$(CAI_SMHI_LOCATION)")

run-mcp-server:
	$(call run_c_example,cai_example_mcp_server,--port "$(CAI_MCP_EXAMPLE_PORT)")

run-searxng-search:
	$(call run_c_example,cai_example_searxng_search,"$(CAI_SEARXNG_QUERY)")

run-terminal-chat:
	$(call run_c_example,cai_example_terminal_chat,$(TERMINAL_CHAT_ARGS))

run-mike-mind:
	$(call run_c_example,cai_example_mike_mind,)

run-lua-chatgpt-login:
	$(call run_lua_example,examples/lua-chatgpt-login/main.lua,$(CHATGPT_LOGIN_ARGS))

run-lua-basic:
	$(call run_lua_example,examples/lua-basic/main.lua,)

run-lua-terminal-chat:
	$(call run_lua_example,examples/lua-terminal-chat/main.lua,$(TERMINAL_CHAT_ARGS))

run-lua-conversation:
	$(call run_lua_example,examples/lua-conversation/main.lua,)

run-lua-session-state:
	$(call run_lua_example,examples/lua-session-state/main.lua,"$(CAI_LUA_SESSION_STATE_PATH)")
