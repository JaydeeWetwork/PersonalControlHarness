# PROPOSED fair-play test beds for the PCH boss (Herominde, 2026-10-22 7:30 PM ET)
Status: proposals only. Cortana implements if JD approves. Nothing here has been run.

1. **Thread-count determinism.** Run the same sealed tick with 1, 2, 4, and N threads. Pass if the chosen branch and the final receipt hash match exactly.
2. **Telegraph test (Boardy's question).** Every committed attack must show a wind-up of at least K frames on the rendered HUD before it lands. Pass if 100% of attacks in the receipt have a visible tell logged before impact.
3. **HUD-only honesty.** Hide or blank the energy bar on screen while the real value stays full. Pass if the boss acts as though its energy is unknown (no x-ray of internal state).
4. **Human input rate.** Cap actions at a human rate (for example ≤ 10 per second, with inventory swaps taking the same time a player's do). Pass if no burst in the receipt exceeds the cap.
5. **Fixed branch count (ranked mode).** Run the same fight on a "phone" budget and a "PC" budget with the branch count locked. Pass if the decisions are identical.
6. **Brain-version stamp.** Every receipt carries the boss version. Replay a v1 receipt on a v2 build. Pass if it refuses or labels the mismatch instead of silently diverging.
