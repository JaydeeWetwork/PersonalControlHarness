"""Boss / fair-play test beds not yet implemented (Master Chief TB1/TB2, Herominde #2/#3).
They need a HUD / telegraph observation feed that the PCH reference boss stub
does not model yet, so they are registered and SKIPPED with a reason. Owner
decision: Implement (testbeds/DECISIONS.md). Designs: testbeds/designs/."""
import sys

import pch_testlib

REASONS = {
    "hud_reading": "Master Chief TB1: needs a boss brain + HUD feed in PCH (not present); decision: Implement, see testbeds/DECISIONS.md",
    "telegraph_fairness": "Master Chief TB2 / Herominde #2: needs boss attacks with wind-up/HUD ticks (not present); decision: Implement, see testbeds/DECISIONS.md",
    "ranked_fixed_branch_count": "Herominde #5: locked branch-count check not yet wired into this bed; decision: Implement, see testbeds/DECISIONS.md",
    "brain_version_stamp": "Herominde #6: refuse-or-label replay check not yet wired into this bed; decision: Implement, see testbeds/DECISIONS.md",
    "hud_only_honesty": "Herominde #3: needs a HUD-only observation path (not present); decision: Implement, see testbeds/DECISIONS.md",
}
pch_testlib.skip(REASONS.get(sys.argv[1], "unknown test bed"))
