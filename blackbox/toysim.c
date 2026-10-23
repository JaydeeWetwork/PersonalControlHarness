#include <string.h>
#include "toysim.h"
static uint64_t mix(uint64_t z) { z += 0x9e3779b97f4a7c15ull; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull; return z ^ (z >> 31); }
void toysim_init(toysim *s, uint64_t seed) {
  s->seed = seed;
  for (int i = 0; i < TS_MOBS; i++) { s->x[i] = (int32_t)(mix(seed + i) % 200) - 100; s->hp[i] = 10; }
}
int toysim_tick(toysim *s, uint64_t tick, const ts_input *in, ts_decision out[TS_MOBS]) {
  int n = 0;
  memset(out, 0, sizeof(ts_decision) * TS_MOBS); /* struct padding must be zero: it is hashed and compared */
  for (int i = 0; i < TS_MOBS; i++) {
    uint64_t r = mix(s->seed ^ (tick * 1315423911ull) ^ (uint64_t)i);
    int32_t d = in->player_x - s->x[i];
    int32_t ad = d < 0 ? -d : d;
    uint8_t a;
    if (s->hp[i] < 4) a = TS_FLEE;
    else if (ad + in->noise % 3 < 20) a = TS_CHASE;
    else if (r % 4 == 0) a = TS_WANDER;
    else a = TS_IDLE;
    if (a == TS_CHASE) { s->x[i] += d > 0 ? 1 : (d < 0 ? -1 : 0); if (ad <= 1) s->hp[i] -= 1; }
    else if (a == TS_FLEE) { s->x[i] += d > 0 ? -2 : 2; if (r % 16 == 0) s->hp[i] = 10; }
    else if (a == TS_WANDER) s->x[i] += (r >> 8) % 2 ? 1 : -1;
    out[n].mob = (uint8_t)i; out[n].action = a; out[n].new_x = s->x[i]; n++;
  }
  return n;
}
ts_input toysim_live_input(uint64_t seed, uint64_t tick) {
  ts_input in;
  uint64_t r = mix(seed * 31 + tick);
  in.player_x = (int32_t)((tick / 4) % 160) - 80 + (int32_t)(r % 5) - 2;
  in.noise = (int32_t)((r >> 16) % 7);
  return in;
}
