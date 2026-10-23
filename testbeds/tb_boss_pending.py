"""Boss / fair-play test beds 08, 09, 11 (Master Chief TB1/TB2, Herominde #2/#3).
They need a HUD / telegraph observation feed that the PCH reference boss stub
does not model yet, so they are registered and SKIPPED with a reason. Owner
decision: Implement (testbeds/DECISIONS.md). Designs: testbeds/designs/."""
import sys

import pch_testlib

REASONS = {
}
pch_testlib.skip(REASONS.get(sys.argv[1], "unknown test bed"))
