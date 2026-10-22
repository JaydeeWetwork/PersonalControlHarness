# License Options — DECIDED: MIT (JD, 2026-10-19)

> **DECISION RECORD.** JD chose the **MIT License** for the combined
> release (LithoLattice + LithoKraft + PCH) on 2026-10-19, in his words
> because he "wanted to shake hands with the smart people." The root
> `LICENSE` carries the MIT text. This file is retained as the record
> of the options weighed. Publication of the release remains a separate
> pending decision.
>
> Note: `pch/LICENSE` currently contains an MIT text carried over from
> the standalone PCH package as a marked working draft. If the combined
> release adopts a different license, that file is replaced so the repo
> carries exactly one license, chosen once, at the root.

## Option 1 — MIT

The maximal-spread permissive license: anyone may use, copy, modify, and
redistribute, including commercially and inside closed products, with
only attribution required.

- **Consequence in one line:** widest adoption and easiest verification
  by strangers — and zero protection for the Timing Handshake method;
  anyone may take the code proprietary.

## Option 2 — Apache License 2.0

Permissive like MIT, with an express patent grant from contributors and
a patent-retaliation clause, plus NOTICE / change-marking requirements.

- **Consequence in one line:** nearly the same spread as MIT, with the
  patent position stated instead of implied — still fully open,
  including for commercial closed use of the code itself.

## Option 3 — BSL 1.1-style delayed open (source-available)

The Business Source License pattern: the source is public now — anyone
may read, build, and verify it — but production use is restricted until
a stated Change Date, after which the whole tree automatically converts
to a real open-source license (e.g., Apache 2.0). This matches JD's
stated doctrine of 2026-10-15: protect the Timing Handshake for a first
term of years, then open it.

- **Consequence in one line:** the verifiable claim is fully public from
  day one and the method is protected during the term — but the release
  is "source-available," not open source, until the change date arrives.

## What the choice does not change

Under every option: the claims in `README.md` remain independently
verifiable from this tree; the Pillars (`pch/PILLARS.md`) travel with
the code; and third-party components keep their own licenses (see
`engine/THIRD_PARTY_LICENSES.md`).

**Decision: JD's, pending.**
