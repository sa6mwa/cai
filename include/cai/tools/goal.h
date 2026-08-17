/** @file cai/tools/goal.h
 *  Durable Codex-style goal tools for an active CAI agent session.
 */
#ifndef CAI_TOOLS_GOAL_H
#define CAI_TOOLS_GOAL_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAI_GOAL_GET_TOOL_NAME "get_goal"
#define CAI_GOAL_CREATE_TOOL_NAME "create_goal"
#define CAI_GOAL_UPDATE_TOOL_NAME "update_goal"
#define CAI_GOAL_CLEAR_TOOL_NAME "clear_goal"

/** Register the constrained goal tools against a single session. */
int cai_agent_register_goal_tools(cai_agent *agent, cai_session *session,
                                  cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
