/** @file cai/quota.h ChatGPT subscription usage windows. */
#ifndef CAI_QUOTA_H
#define CAI_QUOTA_H

#include <cai/cai.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cai_chatgpt_quota {
  /** Presence flag for the shortest returned window below one week. */
  int has_short_window;
  long long short_window_seconds;
  /** Remaining quota, 0..100; meaningful only with has_short_window. */
  double short_window_remaining_percent;
  /** Presence flag for a returned seven-day window. */
  int has_weekly;
  double weekly_remaining_percent;
  /** Presence flag for a finite provider credit balance, not currency spend. */
  int has_credit_balance;
  double credit_balance;
  /** Non-zero when credits are unlimited, independently of quota windows. */
  int credits_unlimited;
} cai_chatgpt_quota;

/** Query ChatGPT subscription usage and optional credits for this client.
 * The weekly window may be primary or secondary. A shorter returned window
 * carries its actual duration in seconds. Unavailable fields remain absent.
 * Requires a client with ChatGPT auth; API-key clients return CAI_ERR_INVALID.
 * Each call performs a synchronous request, without a cache or debounce;
 * callers own refresh policy. libcai_app adds its five-minute debounce and
 * background worker. This endpoint may be unavailable for some accounts. */
int cai_client_chatgpt_quota(cai_client *client, cai_chatgpt_quota *out,
                             cai_error *error);

#ifdef __cplusplus
}
#endif

#endif
