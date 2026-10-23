# Test Bed 2: Telegraph Fairness

**Status: Proposed (Master Chief), awaiting JD approval**

## Goal
Make sure every big boss attack gives a fair warning: a visible wind-up plus the energy change on the HUD, early enough that a player can react.

## Setup
- The PCH forks at a sealed tick before each attack and replays it several times.
- Log three things per attack: the wind-up start tick, the energy-drop tick on the HUD, and the hit tick.
- Reaction window target: an adjustable baseline (for example about 250 ms plus input lag), with an easy tier and a hard tier.

## Metrics
- Lead time: ticks (and ms) from the first visible cue to the hit.
- Fairness margin: lead time minus the reaction window.
- Cue consistency: the same attack gets the same wind-up length across branches.
- Unreadable attacks: hits with no wind-up or no HUD energy cue.

## Pass / Fail
- **Pass:** every attack has a fairness margin of 0 or more at its difficulty tier, there are zero unreadable attacks, and wind-up length varies by no more than 1 tick for the same attack.
- **Fail:** any attack lands before the reaction window ends, or any attack has no cue.

## Not run
Not run. This is a design only. No measurements have been taken.
