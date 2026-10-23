/* bbbench - per-entry cost of the Black Box prototype.
 * Measures (1) tick-thread bb_append() latency and (2) end-to-end flusher throughput
 * (hash + write), for several payload sizes. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "blackbox.h"
#include "bb_platform.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void) { return bb_now_s(); }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
int main(void) {
  const uint32_t sizes[] = {16, 64, 256};
  printf("payload | append mean | append p99 | append max | end-to-end per entry | file bytes/entry | dropped\n");
  for (int s = 0; s < 3; s++) {
    uint32_t len = sizes[s];
    const int N = 200000, BATCH = 1000; /* 1000 records per "tick" */
    bb_config c = {0}; c.session_id = 1; c.checkpoint_every = 4096; c.sign = bb_test_sign; c.ring_bytes = 1u << 24;
    bb_writer *w = bb_open("bench.bbx", &c);
    uint8_t pl[256]; memset(pl, 0xA5, sizeof pl);
    double *lat = malloc(sizeof(double) * (N / BATCH));
    double t0 = now();
    for (int b = 0; b < N / BATCH; b++) {
      double a = now();
      for (int i = 0; i < BATCH; i++) bb_append(w, (uint64_t)b, BB_T_DECISION, pl, len);
      lat[b] = (now() - a) / BATCH;
    }
    uint64_t dropped = bb_dropped(w);
    bb_flush(w);
    double t1 = now();
    bb_close(w);
    FILE *f = fopen("bench.bbx", "rb"); fseek(f, 0, SEEK_END); long fb = ftell(f); fclose(f);
    double sum = 0, mx = 0; for (int i = 0; i < N / BATCH; i++) { sum += lat[i]; if (lat[i] > mx) mx = lat[i]; }
    qsort(lat, N / BATCH, sizeof(double), cmpd);
    printf("%4u B  | %8.1f ns | %8.1f ns | %8.1f ns | %10.1f ns | %6.1f (payload %u) | %llu\n", len,
           sum / (N / BATCH) * 1e9, lat[(int)(0.99 * (N / BATCH))] * 1e9, mx * 1e9, (t1 - t0) / N * 1e9,
           (double)(fb - BB_HEADER_SIZE) / N, len, (unsigned long long)dropped);
    free(lat);
  }
  /* raw hash cost for reference */
  uint8_t buf[24 + 32 + 64], o[32]; memset(buf, 1, sizeof buf);
  double a = now(); for (int i = 0; i < 1000000; i++) { buf[0] = (uint8_t)i; bb_sha256(buf, 24 + 32 + 64, o); }
  printf("reference: SHA-256 over one 64 B-payload entry (prev+hdr+payload = 120 B): %.1f ns\n", (now() - a) * 1e3);
  remove("bench.bbx");
  return 0;
}
