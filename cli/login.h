#ifndef CAI_CLI_LOGIN_H
#define CAI_CLI_LOGIN_H

/* Run browser login and persist ChatGPT auth in cai state, or at path. */
int cai_cli_login(const char *auth_json_path);

#endif
