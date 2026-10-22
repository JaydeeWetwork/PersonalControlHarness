# Publishing This Release to GitHub — Step by Step

JD — this folder **is** the repository, complete and ready. Nothing in
it needs building or fixing first. You publish it with **GitHub
Desktop**, signed in as yourself, by your own hand. This guide assumes
it's your first time with GitHub Desktop; every step is spelled out.

**What you're publishing:** the LithoLattice engine, the LithoKraft
game, the Timing Handshake, and the Personal Control Harness — under
the **MIT License** (chosen 2026-10-19). About 1,845 files, ~15 MB.

**Before you start:** you need a GitHub account, and GitHub Desktop
installed and signed in to that account (download it from
`desktop.github.com` if it isn't installed yet).

---

## Part 1 — Download and extract

1. Download the release zip to your Windows machine.
2. Right-click the zip → **Extract All…** → pick a folder you'll
   remember, e.g. `Documents\litholattice-release` → **Extract**.
3. Open the extracted folder. You should see, side by side:
   `LICENSE`, `README.md`, `MANIFEST.md`, `PUBLISH-GUIDE.md` (this
   file), and the folders `engine`, `game`, `pch`, `docs`, `tools`.
4. If instead you see one single folder inside, open it — use the
   **inner** folder, the one that has `LICENSE` sitting in it. That
   inner folder is the repository root; it's the one everything below
   points at.

## Part 2 — Optional prep check (pick one, or skip)

**Option A — double-click `prep-windows.bat`** inside the extracted
folder. It speaks plainly and pauses at the end. It will:

- check the folder has everything it should (`LICENSE`, `README.md`,
  `MANIFEST.md`, `engine\`, `game\`, `pch\`) and stop with an
  explanation if something's missing;
- count the files so you can see they're all there;
- **if** the Git command-line tools happen to be installed, run
  `git init` and make the first commit for you (using *your own*
  configured Git identity — if you haven't set one, it prints the two
  `git config` commands you need and stops before committing, no harm
  done);
- **if** Git isn't installed, say so and exit cleanly — GitHub Desktop
  handles it all in Part 3 instead.

`prep-windows.ps1` does exactly the same thing for PowerShell. If
Windows blocks the `.ps1` (execution policy), just use the `.bat` —
they're twins. Both scripts are plain text, short, and safe to read in
Notepad first: they delete nothing and touch no network.

**Option B — skip the scripts entirely.** GitHub Desktop does
everything in Part 3. The scripts are a convenience, not a requirement.

## Part 3 — Add the folder in GitHub Desktop

1. Open **GitHub Desktop**.
2. **File → Add Local Repository…**
3. Click **Choose…**, pick the extracted folder (the one with
   `LICENSE` in it), then **Add Repository**.
4. If Desktop says the folder **"is not a Git repository"**, click the
   blue **"create a repository here"** link in that message, then
   **Create Repository**. The defaults it offers are fine.
5. The left panel fills with a list of new files — roughly eighteen
   hundred. That's expected: it's the whole release, waiting for its
   first commit.

*(If you ran the prep script and it already made the first commit,
Desktop instead shows "No local changes" — skip Part 4 and go
straight to Part 5.)*

## Part 4 — Pre-publish checklist (read this before Part 5)

- [ ] **`LICENSE` is at the folder root.** It is the MIT License,
      © David Rittenhouse / Wet Wafer Works Games Incorporated.
      GitHub reads this file and displays the repo as MIT-licensed.
- [ ] **The signing-key note — your call.** One file,
      `engine/src/assets/mod_signature.cpp`, has a test key baked
      into it: a *retired throwaway **public** key* from development.
      A public key is not a secret, and there is **no private key
      anywhere in this package** (verified by scan — see `MANIFEST.md`,
      "Security and privacy scan", for the full note). Your real
      dev signing-key decision is still pending. Publishing now means
      the world sees the tree still trusting that test key. Publish
      as-is, or hold until the real key replaces it — your decision,
      made knowingly.
- [ ] **No personal data.** The package was scanned before packing:
      no private keys, no credential files, no `.env` files, no
      personal documents, no private domain names. A few internal
      memos were deliberately withheld; they're listed with reasons
      in `MANIFEST.md`.
- [ ] **Integrity (optional).** `SHA256SUMS.txt` in the folder root
      lists SHA-256 hashes of 12 key files. To spot-check one in
      PowerShell: `Get-FileHash .\README.md` and compare with the
      line for `README.md` in `SHA256SUMS.txt`.

## Part 5 — The first commit

1. At the bottom-left of GitHub Desktop is a box labeled
   **"Summary (required)"**. Paste this exactly:

   ```
   Initial release: LithoLattice engine, LithoKraft game, Timing Handshake, Personal Control Harness — MIT
   ```

2. The description box below it is optional — leave it empty.
3. Click **Commit to main**.

## Part 6 — Publish

1. Click the blue **Publish Repository** button near the top of the
   GitHub Desktop window.
2. **Name:** `litholattice` (suggested — it must be unique on your
   account; if GitHub says it's taken, it will offer a variant).
3. **Description** (optional): `LithoLattice engine + LithoKraft game
   + Timing Handshake + Personal Control Harness. MIT.`
4. ⚠️ **Leave "Keep this code private" UNCHECKED.** Checked means a
   private repository. Unchecked is what makes this the public
   release.
5. Click **Publish Repository**, and let the progress bar finish
   (~15 MB — quick).
6. One honest sentence before you click: once it's public, anyone can
   copy and fork it, forever — that can't be undone. That's the
   point of this release, and the click is yours.

## Part 7 — After publishing, check the page

Click **View on GitHub** (or open your profile and pick the repo) and
confirm:

- The **README renders** below the file list, titled
  "LithoLattice · LithoKraft · Personal Control Harness".
- The **file list** shows `engine/`, `game/`, `pch/`, `docs/`,
  `tools/`, `LICENSE`, `MANIFEST.md` at the top level.
- The **About** panel (right side) shows the **MIT license**.
- The latest commit shows **your summary** from Part 5.

If all four are there, the release is live and whole. Post it
everywhere.

---

### Notes for Windows

- **`engine\LithoKraft` is a symbolic link** in the original tree,
  pointing at `game\LithoKraft` (the build expects it there).
  Windows extraction may show it as a tiny 18-byte file instead of a
  folder. Nothing is missing — the complete game source is in
  `game\LithoKraft`.
- If Windows SmartScreen questions the `.bat`, choose **More info →
  Run anyway** — or read the script in Notepad first; it's short,
  commented, and does nothing but check, count, and (if Git exists)
  init + commit.
- Troubled by a step? The checklist in Part 4 and the file list in
  Part 1 are the ground truth: `LICENSE`, `README.md`, `MANIFEST.md`,
  `engine\`, `game\`, `pch\` — if those six are in the folder you're
  standing in, you're in the right place.
