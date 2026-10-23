/* bbtest - self-tests for the Black Box prototype. Exit 0 = all pass.
 * Also PCH test bed 16 (Black Box replay + tamper), ctest pch.blackbox.replay_tamper.
 * Signatures here use the TEST-ONLY HMAC signer (public key): never a real key. */
#define _POSIX_C_SOURCE 200809L
#include "blackbox.h"
#include "sha256.h"
#include "toysim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *slurp(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(*n ? *n : 1); if (fread(b, 1, *n, f) != *n) { free(b); b = NULL; } fclose(f); return b;
}
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Record a toy-sim run: per tick one INPUT, one SENSOR, TS_MOBS DECISIONs. */
static void record_run(const char *path, uint64_t seed, int ticks, uint32_t ckpt_every) {
  bb_config c = {0}; c.session_id = 0xB1ACB0C5ull; c.checkpoint_every = ckpt_every; c.sign = bb_test_sign;
  bb_writer *w = bb_open(path, &c);
  toysim s; toysim_init(&s, seed);
  bb_append(w, 0, BB_T_SEED, &seed, sizeof seed);
  for (int t = 1; t <= ticks; t++) {
    ts_input in = toysim_live_input(seed, t);
    ts_decision d[TS_MOBS];
    bb_append(w, t, BB_T_INPUT, &in.player_x, sizeof in.player_x);
    bb_append(w, t, BB_T_SENSOR, &in.noise, sizeof in.noise);
    int n = toysim_tick(&s, t, &in, d);
    for (int i = 0; i < n; i++) bb_append(w, t, BB_T_DECISION, &d[i], sizeof d[i]);
  }
  bb_close(w);
}

/* ---- replay: feed recorded inputs to a fresh sim, compare decisions tick by tick ---- */
typedef struct { toysim s; int have_seed; ts_input in; uint64_t tick; ts_decision exp[TS_MOBS]; int nexp, ndec;
  uint64_t ticks, mismatches, first_bad_tick; } replay_ctx;
static void replay_finish_tick(replay_ctx *r) {
  if (!r->tick) return;
  ts_decision got[TS_MOBS];
  int n = toysim_tick(&r->s, r->tick, &r->in, got);
  r->ticks++;
  if (n != r->ndec || memcmp(got, r->exp, sizeof(ts_decision) * n)) { if (!r->mismatches) r->first_bad_tick = r->tick; r->mismatches++; }
  r->ndec = 0; r->tick = 0;
}
static int replay_cb(void *ctx, const bb_entry *e) {
  replay_ctx *r = ctx;
  if (e->type == BB_T_SEED) { uint64_t sd; memcpy(&sd, e->payload, 8); toysim_init(&r->s, sd); r->have_seed = 1; return 0; }
  if (e->tick != r->tick) { replay_finish_tick(r); r->tick = e->tick; }
  if (e->type == BB_T_INPUT) memcpy(&r->in.player_x, e->payload, 4);
  else if (e->type == BB_T_SENSOR) memcpy(&r->in.noise, e->payload, 4);
  else if (e->type == BB_T_DECISION && r->ndec < TS_MOBS) memcpy(&r->exp[r->ndec++], e->payload, sizeof(ts_decision));
  return 0;
}
static bb_verify_code replay_buf(const uint8_t *b, size_t n, replay_ctx *r, bb_verify_report *rep, int sig) {
  memset(r, 0, sizeof *r);
  bb_verify_code c = bb_verify_buffer(b, n, sig ? bb_test_verify : NULL, NULL, replay_cb, r, rep);
  replay_finish_tick(r);
  return c;
}

/* Forger: rewrite every entry hash after editing (cannot produce checkpoint signatures). */
static void rechain(uint8_t *b, size_t n) {
  uint8_t prev[32]; bb_sha256(b, BB_HEADER_SIZE, prev);
  size_t off = BB_HEADER_SIZE;
  while (off + BB_ENTRY_HDR_SIZE <= n) {
    uint32_t len = get32(b + off);
    if (b[off + 4] == BB_T_CHECKPOINT) memcpy(b + off + BB_ENTRY_HDR_SIZE + 8, prev, 32); /* fix covered hash too */
    bb_sha256_ctx c; bb_sha256_init(&c); bb_sha256_update(&c, prev, 32);
    bb_sha256_update(&c, b + off, BB_ENTRY_HDR_SIZE + len); bb_sha256_final(&c, prev);
    memcpy(b + off + BB_ENTRY_HDR_SIZE + len, prev, 32);
    off += BB_ENTRY_HDR_SIZE + len + 32;
  }
}

static void test_sha256(void) {
  printf("[sha256 known answers]\n");
  uint8_t o[32]; char hex[65];
  bb_sha256("abc", 3, o); for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", o[i]);
  CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256(abc)=%s", hex);
  bb_sha256("", 0, o); for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", o[i]);
  CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "sha256('')=%s", hex);
  const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  bb_sha256(m, strlen(m), o); for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", o[i]);
  CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256(448-bit)=%s", hex);
  /* RFC 4231 test case 2 */
  bb_hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, o);
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", o[i]);
  CHECK(!strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "hmac=%s", hex);
}

static void test_append_and_chain(void) {
  printf("[append + chain verification]\n");
  record_run("t_basic.bbx", 42, 100, 64);
  bb_verify_report r;
  bb_verify_code c = bb_verify_file("t_basic.bbx", bb_test_verify, NULL, NULL, NULL, &r);
  uint64_t engine = 1 + 100 * (2 + TS_MOBS);
  uint64_t ckpts = (engine + 63) / 64;
  CHECK(c == BB_V_OK, "verify=%s", bb_verify_code_str(c));
  CHECK(r.entries == engine + ckpts + 1, "entries=%llu want %llu", (unsigned long long)r.entries, (unsigned long long)(engine + ckpts + 1));
  CHECK(r.checkpoints == ckpts, "checkpoints=%llu want %llu", (unsigned long long)r.checkpoints, (unsigned long long)ckpts);
  CHECK(r.last_tick == 100, "last tick %llu", (unsigned long long)r.last_tick);
  CHECK(r.gaps == 0, "unexpected gaps");
  /* same inputs => byte-identical log */
  record_run("t_basic2.bbx", 42, 100, 64);
  size_t n1, n2; uint8_t *a = slurp("t_basic.bbx", &n1), *b = slurp("t_basic2.bbx", &n2);
  CHECK(n1 == n2 && !memcmp(a, b, n1), "two identical runs produced different logs");
  free(a); free(b);
  /* invalid appends are rejected, not written */
  bb_config cf = {0}; bb_writer *w = bb_open("t_inval.bbx", &cf);
  CHECK(bb_append(w, 1, BB_T_END, NULL, 0) == BB_EINVAL, "reserved type accepted");
  CHECK(bb_append(w, 1, BB_T_INPUT, NULL, 4) == BB_EINVAL, "NULL payload accepted");
  CHECK(bb_append(w, 1, BB_T_INPUT, "x", BB_MAX_PAYLOAD + 1) == BB_EINVAL, "oversize accepted");
  CHECK(bb_close(w) == BB_OK, "close");
  CHECK(bb_verify_file("t_inval.bbx", NULL, NULL, NULL, NULL, &r) == BB_V_OK && r.entries == 1, "empty log should be just END");
}

static void test_byte_flips(void) {
  printf("[tamper: every single-byte flip, every bit position cycled]\n");
  record_run("t_flip.bbx", 7, 20, 16);
  size_t n; uint8_t *b = slurp("t_flip.bbx", &n);
  bb_verify_report r;
  CHECK(bb_verify_buffer(b, n, bb_test_verify, NULL, NULL, NULL, &r) == BB_V_OK, "baseline");
  size_t undetected = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t mask = (uint8_t)(1u << (i % 8));
    for (int pass = 0; pass < 2; pass++) {
      uint8_t m = pass ? 0xFF : mask;
      b[i] ^= m;
      if (bb_verify_buffer(b, n, bb_test_verify, NULL, NULL, NULL, &r) == BB_V_OK) undetected++;
      b[i] ^= m;
    }
  }
  CHECK(undetected == 0, "%zu of %zu flips undetected", undetected, 2 * n);
  printf("  %zu bytes x 2 flips each = %zu tamper cases, undetected: %zu\n", n, 2 * n, undetected);
  free(b);
}

static void test_truncation(void) {
  printf("[truncation at every length]\n");
  size_t n; uint8_t *b = slurp("t_flip.bbx", &n);
  size_t ok = 0, trunc = 0;
  for (size_t L = 0; L < n; L++) {
    bb_verify_report r;
    bb_verify_code c = bb_verify_buffer(b, L, bb_test_verify, NULL, NULL, NULL, &r);
    if (c == BB_V_OK) ok++;
    if (c == BB_V_TRUNCATED) trunc++;
  }
  CHECK(ok == 0, "%zu truncated lengths verified OK", ok);
  printf("  %zu lengths tried, reported TRUNCATED: %zu, other failure: %zu, wrongly OK: %zu\n", n, trunc, n - trunc - ok, ok);
  /* appended junk after END */
  uint8_t *b2 = malloc(n + 1); memcpy(b2, b, n); b2[n] = 0;
  CHECK(bb_verify_buffer(b2, n + 1, NULL, NULL, NULL, NULL, NULL) != BB_V_OK, "trailing byte accepted");
  /* drop whole tail at an entry boundary: still caught (no END) */
  bb_verify_report r; bb_verify_buffer(b, n, NULL, NULL, NULL, NULL, &r);
  free(b); free(b2);
}

static void test_replay(void) {
  printf("[replay determinism with toy sim]\n");
  record_run("t_replay.bbx", 1234, 2000, 256);
  size_t n; uint8_t *b = slurp("t_replay.bbx", &n);
  replay_ctx rc; bb_verify_report r;
  bb_verify_code c = replay_buf(b, n, &rc, &r, 1);
  CHECK(c == BB_V_OK, "verify %s", bb_verify_code_str(c));
  CHECK(rc.ticks == 2000 && rc.mismatches == 0, "replayed %llu ticks, %llu mismatches (first at %llu)",
        (unsigned long long)rc.ticks, (unsigned long long)rc.mismatches, (unsigned long long)rc.first_bad_tick);
  printf("  replayed %llu ticks, %llu decisions, mismatches: %llu\n", (unsigned long long)rc.ticks,
         (unsigned long long)rc.ticks * TS_MOBS, (unsigned long long)rc.mismatches);

  /* forged decision with the chain recomputed: replay AND checkpoint signature both catch it */
  size_t off = BB_HEADER_SIZE; int edited = 0; uint64_t edit_tick = 0;
  while (off < n) {
    uint32_t len = get32(b + off);
    if (b[off + 4] == BB_T_DECISION && get64(b + off + 16) == 777) {
      ts_decision *d = (ts_decision *)(b + off + BB_ENTRY_HDR_SIZE);
      d->action = (uint8_t)((d->action + 1) % 4); edited = 1; edit_tick = 777; break;
    }
    off += BB_ENTRY_HDR_SIZE + len + 32;
  }
  CHECK(edited, "no decision at tick 777");
  CHECK(bb_verify_buffer(b, n, NULL, NULL, NULL, NULL, &r) == BB_V_HASH_MISMATCH, "naive edit not caught by chain");
  rechain(b, n);
  c = replay_buf(b, n, &rc, &r, 0);
  CHECK(c == BB_V_OK, "rechained log should pass the bare chain check (%s)", bb_verify_code_str(c));
  CHECK(rc.mismatches >= 1 && rc.first_bad_tick == edit_tick, "replay missed forged decision (first bad %llu)", (unsigned long long)rc.first_bad_tick);
  c = bb_verify_buffer(b, n, bb_test_verify, NULL, NULL, NULL, &r);
  CHECK(c == BB_V_BAD_CHECKPOINT, "signed checkpoint missed re-chained forgery: %s", bb_verify_code_str(c));
  printf("  forged decision at tick %llu: replay flagged tick %llu; signed-checkpoint check: %s\n",
         (unsigned long long)edit_tick, (unsigned long long)rc.first_bad_tick, bb_verify_code_str(c));
  free(b);

  /* a changed sim (different code/seed) diverges on replay */
  record_run("t_replay2.bbx", 1234, 50, 0);
  b = slurp("t_replay2.bbx", &n);
  off = BB_HEADER_SIZE; uint64_t other = 999; memcpy(b + off + BB_ENTRY_HDR_SIZE, &other, 8); rechain(b, n);
  replay_buf(b, n, &rc, &r, 0);
  CHECK(rc.mismatches > 0, "wrong seed replay did not diverge");
  free(b);
}

static void test_overflow_gap(void) {
  printf("[ring overflow is never silent]\n");
  bb_config c = {0}; c.ring_bytes = 1u << 18;
  bb_writer *w = bb_open("t_gap.bbx", &c);
  static uint8_t big[60000];
  uint64_t ok = 0, dr = 0;
  for (int i = 0; i < 2000; i++) { bb_status s = bb_append(w, (uint64_t)i, BB_T_SENSOR, big, sizeof big); if (s == BB_OK) ok++; else if (s == BB_DROPPED) dr++; }
  uint64_t wd = bb_dropped(w);
  CHECK(bb_close(w) == BB_OK, "close");
  bb_verify_report r;
  bb_verify_code v = bb_verify_file("t_gap.bbx", NULL, NULL, NULL, NULL, &r);
  CHECK(v == BB_V_OK, "verify %s", bb_verify_code_str(v));
  CHECK(wd == dr && r.dropped == dr, "dropped: append=%llu writer=%llu log=%llu", (unsigned long long)dr, (unsigned long long)wd, (unsigned long long)r.dropped);
  CHECK(r.entries == ok + r.gaps + 1, "entries %llu != ok %llu + gaps %llu + END", (unsigned long long)r.entries, (unsigned long long)ok, (unsigned long long)r.gaps);
  printf("  2000 x 60 KB appends into a 256 KB ring: written %llu, dropped %llu, GAP records %llu (all accounted for)\n",
         (unsigned long long)ok, (unsigned long long)dr, (unsigned long long)r.gaps);
}

/* Bed 16 case 5: reorder / delete / duplicate one entry, raw and with the chain recomputed. */
static size_t entry_offsets(const uint8_t *b, size_t n, size_t *offs, size_t max) {
  size_t k = 0, off = BB_HEADER_SIZE;
  while (off + BB_ENTRY_HDR_SIZE <= n && k < max) { offs[k++] = off; off += BB_ENTRY_HDR_SIZE + get32(b + off) + 32; }
  return k;
}
static void test_reorder_delete_dup(void) {
  printf("[tamper: reorder / delete / duplicate one entry]\n");
  record_run("t_rdd.bbx", 99, 30, 32);
  size_t n; uint8_t *b = slurp("t_rdd.bbx", &n);
  static size_t offs[4096]; size_t ne = entry_offsets(b, n, offs, 4096);
  uint8_t *m = malloc(2 * n);
  size_t cases = 0, caught = 0, wrong_code = 0;
  for (size_t i = 1; i + 2 < ne; i++) {          /* skip SEED (0) and keep END last */
    size_t a = offs[i], bnd = offs[i + 1], c2 = offs[i + 2];
    size_t la = bnd - a, lb = c2 - bnd;
    for (int op = 0; op < 3; op++) {
      size_t mn = 0;
      if (op == 0) { memcpy(m, b, a); memcpy(m + a, b + bnd, lb); memcpy(m + a + lb, b + a, la); memcpy(m + c2, b + c2, n - c2); mn = n; }
      else if (op == 1) { memcpy(m, b, a); memcpy(m + a, b + bnd, n - bnd); mn = n - la; }
      else { memcpy(m, b, bnd); memcpy(m + bnd, b + a, la); memcpy(m + bnd + la, b + bnd, n - bnd); mn = n + la; }
      for (int re = 0; re < 2; re++) {
        if (re) rechain(m, mn);
        bb_verify_report r;
        bb_verify_code v = bb_verify_buffer(m, mn, bb_test_verify, NULL, NULL, NULL, &r);
        cases++;
        if (v != BB_V_OK) caught++;
        if (v != BB_V_BAD_SEQ && v != BB_V_HASH_MISMATCH) wrong_code++;
      }
    }
  }
  CHECK(cases > 0 && caught == cases, "%zu of %zu reorder/delete/duplicate cases undetected", cases - caught, cases);
  CHECK(wrong_code == 0, "%zu cases reported a code other than BAD_SEQ / HASH_MISMATCH", wrong_code);
  printf("  %zu cases (3 ops x raw/re-chained over %zu entries), detected: %zu, other code: %zu\n", cases, ne, caught, wrong_code);
  free(m); free(b);
}

int main(void) {
  test_sha256();
  test_append_and_chain();
  test_byte_flips();
  test_truncation();
  test_replay();
  test_overflow_gap();
  test_reorder_delete_dup();
  printf("\n%d checks, %d failed: %s\n", checks, fails, fails ? "FAIL" : "ALL PASS");
  remove("t_basic.bbx"); remove("t_basic2.bbx"); remove("t_inval.bbx"); remove("t_flip.bbx");
  remove("t_replay.bbx"); remove("t_replay2.bbx"); remove("t_gap.bbx"); remove("t_rdd.bbx");
  return fails ? 1 : 0;
}
