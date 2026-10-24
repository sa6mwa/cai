/* Compile as C++ to verify the public declaration resolves to libcai's C ABI.
 */
#ifdef CAI_TEST_INCLUDE_CORE_FIRST
#include <cai/cai.h>
#endif
#include <cai/quota.h>

int main(void) {
  cai_chatgpt_quota quota;
  cai_error error;
  int rc;
  cai_error_init(&error);
  rc = cai_client_chatgpt_quota(NULL, &quota, &error);
  cai_error_cleanup(&error);
  return rc == CAI_ERR_INVALID ? 0 : 1;
}
