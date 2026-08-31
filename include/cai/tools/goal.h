/** @file cai/tools/goal.h
 *  Durable Codex-style goal tools for an active CAI agent session.
 */
#ifndef CAI_TOOLS_GOAL_H
#define CAI_TOOLS_GOAL_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Stable model-facing name for reading the current goal. */
#define CAI_GOAL_GET_TOOL_NAME "get_goal"
/** Stable model-facing name for creating an explicitly requested goal. */
#define CAI_GOAL_CREATE_TOOL_NAME "create_goal"
/** Stable model-facing name for changing a goal's terminal state. */
#define CAI_GOAL_UPDATE_TOOL_NAME "update_goal"
/** Stable model-facing name for clearing a goal. */
#define CAI_GOAL_CLEAR_TOOL_NAME "clear_goal"

/**
 * Register constrained durable goal tools against one agent session. agent and
 * session must be paired and must remain alive while their tool registry lives.
 * The goal state is part of that session's persisted runtime state.
 */
int cai_agent_register_goal_tools(cai_agent *agent, cai_session *session,
                                  cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
