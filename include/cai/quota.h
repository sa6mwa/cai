/** @file cai/quota.h ChatGPT subscription usage windows. */
#ifndef CAI_QUOTA_H
#define CAI_QUOTA_H

#include <cai/cai.h>

typedef struct cai_chatgpt_quota {
  int has_five_hour;
  double five_hour_remaining_percent;
  int has_weekly;
  double weekly_remaining_percent;
} cai_chatgpt_quota;

/** Query the ChatGPT subscription usage endpoint for this authenticated client.
 * Each unavailable window remains absent. This endpoint is provider owned and
 * may be unavailable for some accounts; callers should hide absent values. */
int cai_client_chatgpt_quota(cai_client *client, cai_chatgpt_quota *out,
                             cai_error *error);

#endif
