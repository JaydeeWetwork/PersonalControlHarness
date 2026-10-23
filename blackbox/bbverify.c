/* bbverify - verify a Black Box log: chain, checkpoints, truncation.
 * usage: bbverify [--test-key] [--dump] FILE
 *   --test-key  check checkpoint signatures with the prototype's TEST signer
 *   --dump      print each verified engine record (seq, tick, type, len)
 * exit: 0 = OK, 1 = verification failed, 2 = usage error */
#include "blackbox.h"
#include <stdio.h>
#include <string.h>
static int dump(void *ctx, const bb_entry *e) { (void)ctx;
  printf("seq=%llu tick=%llu type=0x%02x len=%u\n", (unsigned long long)e->seq, (unsigned long long)e->tick, e->type, e->len);
  return 0; }
int main(int argc, char **argv) {
  int tk = 0, dp = 0; const char *path = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--test-key")) tk = 1; else if (!strcmp(argv[i], "--dump")) dp = 1;
    else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; } else path = argv[i];
  }
  if (!path) { fprintf(stderr, "usage: bbverify [--test-key] [--dump] FILE\n"); return 2; }
  bb_verify_report r;
  bb_verify_code c = bb_verify_file(path, tk ? bb_test_verify : NULL, NULL, dp ? dump : NULL, NULL, &r);
  printf("%s: %s\n entries verified: %llu (last tick %llu)\n signed checkpoints verified: %llu (trusted through seq %llu)\n"
         " gaps: %llu (records dropped: %llu)\n last good offset: %llu\n head: ",
         path, bb_verify_code_str(c), (unsigned long long)r.entries, (unsigned long long)r.last_tick,
         (unsigned long long)r.checkpoints, (unsigned long long)r.last_signed_seq, (unsigned long long)r.gaps,
         (unsigned long long)r.dropped, (unsigned long long)r.last_good_offset);
  for (int i = 0; i < 32; i++) printf("%02x", r.head[i]);
  printf("\n");
  if (!tk) printf(" note: checkpoint signatures NOT checked (no --test-key)\n");
  return c == BB_V_OK ? 0 : 1;
}
