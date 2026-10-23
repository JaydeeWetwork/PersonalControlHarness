/* blackbox.c - see blackbox.h. C11; threads via bb_platform.h (POSIX or Windows). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdlib.h>
#include "bb_platform.h"
#include "blackbox.h"
#include "sha256.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LITHO_VERBOSE_LOG
#define BB_VLOG(...) fprintf(stderr, "[blackbox] " __VA_ARGS__)
#else
#define BB_VLOG(...) ((void)0)
#endif
/* Critical log: always on (release too). */
#define BB_CRIT(...) fprintf(stderr, "[blackbox CRITICAL] " __VA_ARGS__)

#define RING_REC_HDR 16 /* u32 len, u16 type, u16 pad, u64 tick */
#define CKPT_PAYLOAD (8 + 32 + 8 + BB_SIG_SIZE)

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

struct bb_writer {
  /* producer side */
  _Alignas(64) _Atomic uint64_t head; /* bytes written by producer */
  uint64_t produced;                  /* records accepted (producer-private) */
  _Atomic uint64_t produced_pub;
  _Atomic uint64_t dropped;
  _Atomic uint64_t drop_first_tick, drop_last_tick;
  /* consumer side */
  _Alignas(64) _Atomic uint64_t tail;
  _Atomic uint64_t consumed;
  _Atomic int stop;
  _Atomic int io_error;
  uint8_t *ring;
  size_t cap, mask;
  FILE *f;
  bb_thread thr;
  bb_config cfg;
  /* chain state (flusher thread only) */
  uint8_t prev[32];
  uint64_t seq;
  uint64_t since_ckpt;
  uint64_t gap_reported;
  uint64_t last_tick;
};

/* ---------- chain writer (flusher thread) ---------- */
static void emit(bb_writer *w, uint64_t tick, uint16_t type, const uint8_t *pl, uint32_t len) {
  uint8_t hdr[BB_ENTRY_HDR_SIZE];
  put32(hdr, len); put16(hdr + 4, type); put16(hdr + 6, 0);
  put64(hdr + 8, w->seq); put64(hdr + 16, tick);
  bb_sha256_ctx c; bb_sha256_init(&c);
  bb_sha256_update(&c, w->prev, 32); bb_sha256_update(&c, hdr, sizeof hdr);
  if (len) bb_sha256_update(&c, pl, len);
  bb_sha256_final(&c, w->prev);
  if (fwrite(hdr, 1, sizeof hdr, w->f) != sizeof hdr || (len && fwrite(pl, 1, len, w->f) != len) ||
      fwrite(w->prev, 1, 32, w->f) != 32) {
    if (!atomic_exchange(&w->io_error, 1)) BB_CRIT("write failed at seq %llu\n", (unsigned long long)w->seq);
  }
  w->seq++;
  w->last_tick = tick;
}

static void ckpt_msg(uint64_t session, uint64_t covers, const uint8_t prev[32], uint8_t out[32]) {
  uint8_t b[10 + 8 + 8 + 32];
  memcpy(b, "LLBB-CKPT1", 10); put64(b + 10, session); put64(b + 18, covers); memcpy(b + 26, prev, 32);
  bb_sha256(b, sizeof b, out);
}

static void emit_checkpoint(bb_writer *w) {
  uint8_t pl[CKPT_PAYLOAD] = {0}, msg[32];
  uint64_t covers = w->seq - 1; /* last entry covered */
  put64(pl, covers); memcpy(pl + 8, w->prev, 32);
  if (w->cfg.sign) {
    ckpt_msg(w->cfg.session_id, covers, w->prev, msg);
    if (w->cfg.sign(w->cfg.sign_ctx, msg, pl + 48, pl + 40) != 0) BB_CRIT("checkpoint signer failed\n");
  }
  emit(w, w->last_tick, BB_T_CHECKPOINT, pl, sizeof pl);
  fflush(w->f);
  if (w->cfg.fsync_on_checkpoint) bb_fsync(w->f);
  w->since_ckpt = 0;
  BB_VLOG("checkpoint covers seq %llu\n", (unsigned long long)covers);
}

static void report_gap(bb_writer *w) {
  uint64_t d = atomic_load(&w->dropped);
  if (d == w->gap_reported) return;
  uint8_t pl[24];
  put64(pl, d - w->gap_reported);
  put64(pl + 8, atomic_load(&w->drop_first_tick)); put64(pl + 16, atomic_load(&w->drop_last_tick));
  BB_CRIT("ring overflow: %llu records dropped\n", (unsigned long long)(d - w->gap_reported));
  w->gap_reported = d;
  atomic_store(&w->drop_first_tick, 0);
  emit(w, w->last_tick, BB_T_GAP, pl, sizeof pl);
}

static void ring_read(bb_writer *w, uint64_t pos, uint8_t *dst, size_t n) {
  size_t o = pos & w->mask, k = w->cap - o;
  if (k > n) k = n;
  memcpy(dst, w->ring + o, k);
  if (n > k) memcpy(dst + k, w->ring, n - k);
}

static int drain(bb_writer *w) {
  static _Thread_local uint8_t pl[BB_MAX_PAYLOAD];
  int any = 0;
  uint64_t t = atomic_load_explicit(&w->tail, memory_order_relaxed);
  uint64_t h = atomic_load_explicit(&w->head, memory_order_acquire);
  while (t != h) {
    uint8_t rh[RING_REC_HDR];
    ring_read(w, t, rh, RING_REC_HDR);
    uint32_t len = get32(rh);
    ring_read(w, t + RING_REC_HDR, pl, len);
    emit(w, get64(rh + 8), get16(rh + 4), pl, len);
    t += (RING_REC_HDR + len + 7) & ~(uint64_t)7;
    atomic_store_explicit(&w->tail, t, memory_order_release);
    atomic_fetch_add(&w->consumed, 1);
    any = 1;
    if (w->cfg.checkpoint_every && ++w->since_ckpt >= w->cfg.checkpoint_every) emit_checkpoint(w);
  }
  report_gap(w);
  return any;
}

static void *flusher(void *arg) {
  bb_writer *w = arg;
  /* 0.5 ms poll (1 ms on Windows): the producer never signals, so no wakeup can be lost */
  for (;;) {
    int stop = atomic_load(&w->stop);
    if (!drain(w)) {
      if (stop) break;
      fflush(w->f);
      bb_sleep_us(500);
    }
  }
  return NULL;
}

/* ---------- public writer API ---------- */
bb_writer *bb_open(const char *path, const bb_config *cfg) {
  bb_config c = *cfg;
  if (!c.ring_bytes) c.ring_bytes = 1u << 20;
  if (c.ring_bytes & (c.ring_bytes - 1) || c.ring_bytes < 2 * (BB_MAX_PAYLOAD + RING_REC_HDR)) return NULL;
  bb_writer *w = calloc(1, sizeof *w);
  if (!w) return NULL;
  w->ring = malloc(c.ring_bytes);
  w->f = fopen(path, "wb");
  if (!w->ring || !w->f) { if (w->f) fclose(w->f); free(w->ring); free(w); return NULL; }
  setvbuf(w->f, NULL, _IOFBF, 1 << 16);
  w->cap = c.ring_bytes; w->mask = c.ring_bytes - 1; w->cfg = c;
  uint8_t hdr[BB_HEADER_SIZE] = {0};
  memcpy(hdr, BB_MAGIC, 8); put16(hdr + 8, BB_VERSION); put16(hdr + 10, BB_HASH_SHA256);
  put32(hdr + 12, BB_HEADER_SIZE); put64(hdr + 16, c.session_id); put64(hdr + 24, c.start_tick);
  fwrite(hdr, 1, sizeof hdr, w->f);
  bb_sha256(hdr, sizeof hdr, w->prev); /* genesis = H(header) */
  w->last_tick = c.start_tick;
  if (bb_thread_start(&w->thr, flusher, w) != 0) { fclose(w->f); free(w->ring); free(w); return NULL; }
  return w;
}

bb_status bb_append(bb_writer *w, uint64_t tick, uint16_t type, const void *payload, uint32_t len) {
  if (!w || len > BB_MAX_PAYLOAD || (len && !payload) || type >= 0xF0) return BB_EINVAL;
  uint64_t need = (RING_REC_HDR + len + 7) & ~(uint64_t)7;
  uint64_t h = atomic_load_explicit(&w->head, memory_order_relaxed);
  uint64_t t = atomic_load_explicit(&w->tail, memory_order_acquire);
  if (w->cap - (h - t) < need) {
    uint64_t z = 0;
    atomic_compare_exchange_strong(&w->drop_first_tick, &z, tick ? tick : 1);
    atomic_store(&w->drop_last_tick, tick);
    atomic_fetch_add(&w->dropped, 1);
    return BB_DROPPED;
  }
  uint8_t rh[RING_REC_HDR] = {0};
  put32(rh, len); put16(rh + 4, type); put64(rh + 8, tick);
  size_t o = h & w->mask;
  /* records are 8-aligned and cap >= 2*max, but may wrap: copy in two parts */
  const uint8_t *src[2] = {rh, payload};
  size_t n[2] = {RING_REC_HDR, len};
  for (int i = 0; i < 2; i++) {
    size_t k = w->cap - o; if (k > n[i]) k = n[i];
    memcpy(w->ring + o, src[i], k);
    if (n[i] > k) memcpy(w->ring, src[i] + k, n[i] - k);
    o = (o + n[i]) & w->mask;
  }
  atomic_store_explicit(&w->head, h + need, memory_order_release);
  w->produced++;
  atomic_store_explicit(&w->produced_pub, w->produced, memory_order_relaxed);
  return BB_OK;
}

bb_status bb_flush(bb_writer *w) {
  uint64_t target = atomic_load(&w->produced_pub);
  while (atomic_load(&w->consumed) < target) bb_sleep_us(200);
  bb_sleep_us(200); /* let the flusher finish its fflush pass */
  return atomic_load(&w->io_error) ? BB_EIO : BB_OK;
}

uint64_t bb_dropped(const bb_writer *w) { return atomic_load(&((bb_writer *)w)->dropped); }

bb_status bb_close(bb_writer *w) {
  if (!w) return BB_EINVAL;
  atomic_store(&w->stop, 1);
  bb_thread_join(w->thr);
  if (w->cfg.checkpoint_every && w->since_ckpt) emit_checkpoint(w);
  uint8_t pl[16];
  put64(pl, w->seq); put64(pl + 8, atomic_load(&w->dropped));
  emit(w, w->last_tick, BB_T_END, pl, sizeof pl);
  int err = atomic_load(&w->io_error);
  if (fflush(w->f) != 0) err = 1;
  if (w->cfg.fsync_on_checkpoint) bb_fsync(w->f);
  if (fclose(w->f) != 0) err = 1;
  free(w->ring); free(w);
  return err ? BB_EIO : BB_OK;
}

/* ---------- verification ---------- */
const char *bb_verify_code_str(bb_verify_code c) {
  switch (c) {
    case BB_V_OK: return "OK";
    case BB_V_BAD_HEADER: return "BAD_HEADER";
    case BB_V_HASH_MISMATCH: return "HASH_MISMATCH (entry altered)";
    case BB_V_TRUNCATED: return "TRUNCATED (or writer crashed before END)";
    case BB_V_BAD_SEQ: return "BAD_SEQ";
    case BB_V_BAD_CHECKPOINT: return "BAD_CHECKPOINT";
    case BB_V_TRAILING: return "TRAILING_DATA";
  }
  return "?";
}

bb_verify_code bb_verify_buffer(const uint8_t *b, size_t n, bb_verify_sig_fn vsig, void *vctx,
                                bb_entry_fn on_entry, void *ectx, bb_verify_report *rep) {
  bb_verify_report r; memset(&r, 0, sizeof r);
#define DONE(c) do { r.code = (c); if (rep) *rep = r; return (c); } while (0)
  if (n < BB_HEADER_SIZE || memcmp(b, BB_MAGIC, 8) || get16(b + 8) != BB_VERSION ||
      get16(b + 10) != BB_HASH_SHA256 || get32(b + 12) != BB_HEADER_SIZE)
    DONE(n < BB_HEADER_SIZE && n >= 8 && !memcmp(b, BB_MAGIC, 8) ? BB_V_TRUNCATED : BB_V_BAD_HEADER);
  for (int i = 32; i < BB_HEADER_SIZE; i++) if (b[i]) DONE(BB_V_BAD_HEADER);
  uint64_t session = get64(b + 16);
  uint8_t prev[32];
  bb_sha256(b, BB_HEADER_SIZE, prev);
  size_t off = BB_HEADER_SIZE;
  r.last_good_offset = off;
  int ended = 0;
  while (off < n) {
    if (ended) DONE(BB_V_TRAILING);
    if (n - off < BB_ENTRY_HDR_SIZE) DONE(BB_V_TRUNCATED);
    const uint8_t *h = b + off;
    uint32_t len = get32(h);
    if (len > BB_MAX_PAYLOAD) DONE(BB_V_HASH_MISMATCH); /* corrupted length field */
    if (n - off < (size_t)BB_ENTRY_HDR_SIZE + len + 32) DONE(BB_V_TRUNCATED);
    uint8_t calc[32];
    bb_sha256_ctx c; bb_sha256_init(&c);
    bb_sha256_update(&c, prev, 32); bb_sha256_update(&c, h, BB_ENTRY_HDR_SIZE + len);
    bb_sha256_final(&c, calc);
    if (memcmp(calc, h + BB_ENTRY_HDR_SIZE + len, 32)) DONE(BB_V_HASH_MISMATCH);
    bb_entry e = {get64(h + 8), get64(h + 16), get16(h + 4), get16(h + 6), len, h + BB_ENTRY_HDR_SIZE};
    if (e.seq != r.entries || e.flags) DONE(BB_V_BAD_SEQ);
    if (e.type == BB_T_CHECKPOINT) {
      if (len != CKPT_PAYLOAD || get64(e.payload) != e.seq - 1 || memcmp(e.payload + 8, prev, 32))
        DONE(BB_V_BAD_CHECKPOINT);
      if (vsig) {
        uint8_t msg[32];
        ckpt_msg(session, e.seq - 1, prev, msg);
        if (vsig(vctx, msg, e.payload + 48, e.payload + 40) != 0) DONE(BB_V_BAD_CHECKPOINT);
        r.checkpoints++;
        r.last_signed_seq = e.seq - 1;
      }
    } else if (e.type == BB_T_GAP) {
      if (len != 24) DONE(BB_V_BAD_SEQ);
      r.gaps++; r.dropped += get64(e.payload);
    } else if (e.type == BB_T_END) {
      if (len != 16 || get64(e.payload) != e.seq) DONE(BB_V_BAD_SEQ);
      ended = 1;
    }
    memcpy(prev, calc, 32);
    r.entries++;
    r.last_tick = e.tick;
    off += BB_ENTRY_HDR_SIZE + len + 32;
    r.last_good_offset = off;
    memcpy(r.head, prev, 32);
    if (on_entry && e.type < 0xF0 && on_entry(ectx, &e)) break;
  }
  DONE(ended ? BB_V_OK : BB_V_TRUNCATED);
#undef DONE
}

bb_verify_code bb_verify_file(const char *path, bb_verify_sig_fn vsig, void *vctx,
                              bb_entry_fn on_entry, void *ectx, bb_verify_report *rep) {
  FILE *f = fopen(path, "rb");
  if (!f) { if (rep) { memset(rep, 0, sizeof *rep); rep->code = BB_V_BAD_HEADER; } return BB_V_BAD_HEADER; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(n > 0 ? (size_t)n : 1);
  size_t got = b ? fread(b, 1, (size_t)n, f) : 0;
  fclose(f);
  bb_verify_code c = bb_verify_buffer(b, got, vsig, vctx, on_entry, ectx, rep);
  free(b);
  return c;
}

/* ---------- TEST-ONLY signer ---------- */
static const uint8_t kTestKey[] = "LLBB TEST KEY - NOT A REAL KEY - DO NOT SHIP";
int bb_test_sign(void *ctx, const uint8_t msg[32], uint8_t sig[BB_SIG_SIZE], uint8_t key_id[8]) {
  (void)ctx;
  memset(sig, 0, BB_SIG_SIZE);
  bb_hmac_sha256(kTestKey, sizeof kTestKey - 1, msg, 32, sig);
  memcpy(key_id, "TESTKEY0", 8);
  return 0;
}
int bb_test_verify(void *ctx, const uint8_t msg[32], const uint8_t sig[BB_SIG_SIZE], const uint8_t key_id[8]) {
  (void)ctx;
  uint8_t want[BB_SIG_SIZE] = {0};
  if (memcmp(key_id, "TESTKEY0", 8)) return -1;
  bb_hmac_sha256(kTestKey, sizeof kTestKey - 1, msg, 32, want);
  return memcmp(want, sig, BB_SIG_SIZE) ? -1 : 0;
}
