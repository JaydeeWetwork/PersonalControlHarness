"""Boss / fair-play test beds (Herominde, Master Chief). The public PCH has
no boss brain, HUD feed, input-rate limiter, branch-count setting or
brain-version stamp yet, so these are registered and SKIPPED with a reason.
Designs: testbeds/designs/."""
import sys

import pch_testlib

REASONS = {
    "hud_reading": "Master Chief TB1: needs a boss brain + HUD feed in PCH (not present)",
    "telegraph_fairness": "Master Chief TB2 / Herominde #2: needs boss attacks with wind-up/HUD ticks (not present)",
    "versioned_rematch": "Master Chief TB3: needs brain version stamps + rolling counter in receipts (not present)",
    "human_input_rate": "Herominde #4: needs an action-rate cap on boss controls (not present)",
    "ranked_fixed_branch_count": "Herominde #5: needs a configurable branch-count budget (not present)",
    "brain_version_stamp": "Herominde #6: receipts carry no brain-version field yet",
    "hud_only_honesty": "Herominde #3: needs a HUD-only observation path (not present)",
}
pch_testlib.skip(REASONS.get(sys.argv[1], "unknown test bed"))
