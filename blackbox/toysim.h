/* toysim.h - tiny deterministic "mob AI" sim used to test Black Box replay. */
#ifndef BB_TOYSIM_H
#define BB_TOYSIM_H
#include <stdint.h>
#define TS_MOBS 8
typedef struct { int32_t x[TS_MOBS]; int32_t hp[TS_MOBS]; uint64_t seed; } toysim;
typedef struct { int32_t player_x; int32_t noise; } ts_input;          /* INPUT + SENSOR */
typedef struct { uint8_t mob, action; int32_t new_x; } ts_decision;    /* DECISION */
enum { TS_IDLE = 0, TS_CHASE = 1, TS_FLEE = 2, TS_WANDER = 3 };
void toysim_init(toysim *s, uint64_t seed);
/* One tick: returns number of decisions written to out[TS_MOBS]. Pure function of (state, tick, input). */
int toysim_tick(toysim *s, uint64_t tick, const ts_input *in, ts_decision out[TS_MOBS]);
/* Deterministic input generator for the live run (stands in for a player + sensor). */
ts_input toysim_live_input(uint64_t seed, uint64_t tick);
#endif
