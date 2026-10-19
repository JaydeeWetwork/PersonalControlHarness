// LithoKraft startup script (AngelScript). Runs in a sandbox: only log(),
// string, and functions the game registers (currentDimension()) are available.
void onLoad()
{
  log("Hello from AngelScript! LithoKraft is booting in dimension: " + currentDimension());
}
