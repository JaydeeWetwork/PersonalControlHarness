# Test Bed 1: HUD Reading

**Status: Proposed (Master Chief), awaiting JD approval**

## Goal
Check that the boss brain acts only on what a player could see: its own HUD bars (health, energy), the target's visible bars, and the on-screen controls, inventory controls included. It should never read hidden game state.

## Setup
- A small arena running in the PCH, starting from a sealed tick.
- Scripted dummy player with fixed moves.
- Scenarios: low boss health, empty energy, full energy, item in inventory, and empty inventory.
- A "blind" run where the HUD feed is frozen or blanked. If the brain still reacts to changes, it is reading something it shouldn't.

## Metrics
- Decision match rate: how often the action fits what the HUD shows (for example, it drinks a potion when its health bar is low).
- Hidden-state leaks: actions that change during the blind run.
- Control path: every action goes through an on-screen control press, logged in the receipt.

## Pass / Fail
- **Pass:** zero hidden-state leaks, 100% of actions taken through on-screen controls, and a match rate of at least 90% across scenarios.
- **Fail:** any leak, or any action that skips the controls.

## Not run
Not run. This is a design only. No code exists and no results have been recorded.
