/** @file cai/quota.h ChatGPT subscription usage windows. */
#ifndef CAI_QUOTA_H
#define CAI_QUOTA_H

#include <cai/cai.h>

typedef struct cai_chatgpt_quota {
  int has_short_window;
  long long short_window_seconds;
  double short_window_remaining_percent;
  int has_weekly;
  double weekly_remaining_percent;
  int has_credit_balance;
  double credit_balance;
  int credits_unlimited;
} cai_chatgpt_quota;

/** Query ChatGPT subscription usage and optional credits for this client.
 * The weekly window may be primary or secondary. A shorter returned window
 * carries its actual duration in seconds. Unavailable fields remain absent.
 * This provider-owned endpoint may be unavailable for some accounts. */
int cai_client_chatgpt_quota(cai_client *client, cai_chatgpt_quota *out,
                             cai_error *error);

#endif
