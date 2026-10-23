/* blackbox.h - "Black Box Mystery Solved": tamper-evident, replayable decision log.
 * Standalone C11 prototype for LithoLattice. Not wired into the engine.
 *
 * Model: the tick thread calls bb_append() (memcpy into a lock-free SPSC ring,
 * never blocks, never does I/O). A background flusher thread drains the ring,
 * hash-chains each entry (SHA-256), writes it, and emits periodic checkpoints
 * that a pluggable signer signs. bb_verify_file() re-walks the chain.
 */
#ifndef LL_BLACKBOX_H
#define LL_BLACKBOX_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define BB_MAGIC "LLBLKBX1"
#define BB_VERSION 1
#define BB_HASH_SHA256 1
#define BB_HEADER_SIZE 64
#define BB_ENTRY_HDR_SIZE 24 /* u32 len, u16 type, u16 flags, u64 seq, u64 tick */
#define BB_HASH_SIZE 32
#define BB_SIG_SIZE 64
#define BB_MAX_PAYLOAD 65536u

/* Entry types. 0x01-0x7F: engine/game records. 0xF0+: log-internal records. */
enum {
  BB_T_INPUT = 0x01,      /* player / controller input sample */
  BB_T_SENSOR = 0x02,     /* sensor sample (robotics) */
  BB_T_DECISION = 0x03,   /* AI decision (NPC/mob AI, AI agent) */
  BB_T_SEED = 0x04,       /* RNG seed / world seed */
  BB_T_CHECKPOINT = 0xF0, /* signed checkpoint over the chain so far */
  BB_T_GAP = 0xF1,        /* ring overflowed: N records dropped (never silent) */
  BB_T_END = 0xF2         /* clean close marker */
};

typedef enum {
  BB_OK = 0,
  BB_DROPPED = 1,  /* append: ring full, record dropped and counted */
  BB_EINVAL = -1,
  BB_EIO = -2,
  BB_ENOMEM = -3
} bb_status;

/* Signer: writes a signature over msg (32 bytes) into sig (64 bytes). Return 0 on success.
 * The prototype ships only a TEST signer (HMAC-SHA256 with a fixed test key). */
typedef int (*bb_sign_fn)(void *ctx, const uint8_t msg[32], uint8_t sig[BB_SIG_SIZE], uint8_t key_id[8]);
typedef int (*bb_verify_sig_fn)(void *ctx, const uint8_t msg[32], const uint8_t sig[BB_SIG_SIZE], const uint8_t key_id[8]);

typedef struct {
  uint64_t session_id;
  uint64_t start_tick;
  size_t ring_bytes;          /* power of two; default 1 MiB */
  uint32_t checkpoint_every;  /* entries between checkpoints; 0 = none */
  bb_sign_fn sign;            /* NULL = checkpoints carry no signature */
  void *sign_ctx;
  int fsync_on_checkpoint;    /* 1 = fsync file at each checkpoint */
} bb_config;

typedef struct bb_writer bb_writer;

bb_writer *bb_open(const char *path, const bb_config *cfg);
/* Tick-thread API. Non-blocking, no I/O, no allocation. Single producer. */
bb_status bb_append(bb_writer *w, uint64_t tick, uint16_t type, const void *payload, uint32_t len);
/* Wait until everything appended so far is on disk. NOT for the tick thread. */
bb_status bb_flush(bb_writer *w);
/* Flush, write END, close. Returns BB_OK or BB_EIO. */
bb_status bb_close(bb_writer *w);
uint64_t bb_dropped(const bb_writer *w);

/* ---- verification / reading ---- */
typedef enum {
  BB_V_OK = 0,           /* chain intact and END record present */
  BB_V_BAD_HEADER = 1,
  BB_V_HASH_MISMATCH = 2,/* an entry was altered */
  BB_V_TRUNCATED = 3,    /* file ends mid-entry, or no END record */
  BB_V_BAD_SEQ = 4,
  BB_V_BAD_CHECKPOINT = 5,
  BB_V_TRAILING = 6      /* data after END */
} bb_verify_code;

typedef struct {
  bb_verify_code code;
  uint64_t entries;          /* entries whose hash verified */
  uint64_t checkpoints;      /* checkpoints whose signature verified */
  uint64_t last_good_offset; /* file offset just after the last verified entry */
  uint64_t last_signed_seq;  /* highest seq covered by a verified signed checkpoint */
  uint64_t gaps;             /* GAP records seen */
  uint64_t dropped;          /* total dropped records reported by GAPs */
  uint64_t last_tick;
  uint8_t head[BB_HASH_SIZE];
} bb_verify_report;

typedef struct {
  uint64_t seq, tick;
  uint16_t type, flags;
  uint32_t len;
  const uint8_t *payload;
} bb_entry;

/* Called for each verified entry, in order (may be NULL). Return nonzero to stop. */
typedef int (*bb_entry_fn)(void *ctx, const bb_entry *e);

bb_verify_code bb_verify_buffer(const uint8_t *buf, size_t len, bb_verify_sig_fn vsig, void *vctx,
                                bb_entry_fn on_entry, void *ectx, bb_verify_report *rep);
bb_verify_code bb_verify_file(const char *path, bb_verify_sig_fn vsig, void *vctx,
                              bb_entry_fn on_entry, void *ectx, bb_verify_report *rep);
const char *bb_verify_code_str(bb_verify_code c);

/* TEST-ONLY signer/verifier: HMAC-SHA256 with a fixed, public test key. Not a real signature. */
int bb_test_sign(void *ctx, const uint8_t msg[32], uint8_t sig[BB_SIG_SIZE], uint8_t key_id[8]);
int bb_test_verify(void *ctx, const uint8_t msg[32], const uint8_t sig[BB_SIG_SIZE], const uint8_t key_id[8]);

#ifdef __cplusplus
}
#endif
#endif
