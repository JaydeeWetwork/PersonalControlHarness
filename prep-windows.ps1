# ============================================================
#  LithoLattice combined release - pre-publish prep (PowerShell)
#
#  Run this INSIDE the extracted release folder.
#  Same behavior as prep-windows.bat: structure check, file
#  count, and - if the Git command-line tools are installed -
#  git init and the first commit using YOUR configured
#  identity. It deletes nothing and uses no network. Every
#  action is echoed before it runs.
#
#  If Windows blocks this script (execution policy), just use
#  prep-windows.bat instead - they are twins.
# ============================================================
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot

Write-Host '============================================================'
Write-Host ' LithoLattice combined release - pre-publish prep'
Write-Host " Folder: $PWD"
Write-Host '============================================================'
Write-Host ''

# --- 1. Structure check ---------------------------------------
$missing = @()
foreach ($f in @('LICENSE', 'README.md', 'MANIFEST.md')) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { $missing += "MISSING FILE:   $f" }
}
foreach ($d in @('engine', 'game', 'pch')) {
    if (-not (Test-Path -LiteralPath $d -PathType Container)) { $missing += "MISSING FOLDER: $d" }
}
if ($missing.Count -gt 0) {
    $missing | ForEach-Object { Write-Host " $_" }
    Write-Host ''
    Write-Host ' Structure check FAILED.'
    Write-Host ' This script must run inside the extracted release folder -'
    Write-Host ' the folder that contains LICENSE, README.md and MANIFEST.md.'
    Read-Host ' Press Enter to close'
    exit 1
}
Write-Host ' Structure check: OK - LICENSE, README.md, MANIFEST.md,'
Write-Host ' engine\, game\ and pch\ are all present.'
Write-Host ''

# --- 2. File count ---------------------------------------------
$count = (Get-ChildItem -Recurse -File -Force | Measure-Object).Count
Write-Host " Files in this folder (including this script): $count"
Write-Host ''

# --- 3. Git -----------------------------------------------------
$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) {
    Write-Host ' Git command-line tools were not found on this PC.'
    Write-Host ' No problem - GitHub Desktop will handle init and the'
    Write-Host ' first commit. See PUBLISH-GUIDE.md, Part 3.'
    Read-Host ' Press Enter to close'
    exit 0
}
Write-Host " Git found: $(git --version)"
Write-Host ''

if (Test-Path -LiteralPath '.git' -PathType Container) {
    Write-Host ' A .git folder already exists here - init skipped.'
} else {
    Write-Host ' Running: git init'
    git init
}
Write-Host ''

$uname  = git config user.name
$uemail = git config user.email
if ([string]::IsNullOrWhiteSpace($uname) -or [string]::IsNullOrWhiteSpace($uemail)) {
    Write-Host ' Git does not have your name/email configured yet, so the'
    Write-Host ' first commit was NOT made. Run these two commands once,'
    Write-Host ' then run this script again:'
    Write-Host ''
    Write-Host '     git config --global user.name "Your Name"'
    Write-Host '     git config --global user.email "you@example.com"'
    Write-Host ''
    Write-Host ' (Use the name and email of your GitHub account.)'
    Read-Host ' Press Enter to close'
    exit 0
}
Write-Host " Git identity: $uname <$uemail>"
Write-Host ''
Write-Host ' Running: git add -A'
git add -A
Write-Host ''
Write-Host ' Running: git commit'
git commit -m 'Initial release: LithoLattice engine, LithoKraft game, Timing Handshake, Personal Control Harness - MIT'
Write-Host ''
Write-Host ' Done. Open GitHub Desktop, add this folder as a local'
Write-Host ' repository, and click Publish Repository.'
Write-Host ' See PUBLISH-GUIDE.md, Part 6.'
Read-Host ' Press Enter to close'
