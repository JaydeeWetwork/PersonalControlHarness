// test_env.h: environment probes shared by the engine self-tests.
//
// Some self-tests need an OS capability the machine running them may not
// grant. On Windows, creating symbolic links needs Developer Mode or an
// elevated shell. A test that needs such a capability asks here first. If the
// capability is missing, the test records an honest skip instead of a false
// failure (or a fake pass). The test binary then exits with kSkipReturnCode,
// which CTest reports as "Skipped" (see SKIP_RETURN_CODE in the CMake files).
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace litholattice::test_env {

/// Exit code a self-test binary returns when nothing failed but at least one
/// section was skipped for lack of an OS capability. 77 is the GNU/Automake
/// convention that CTest's SKIP_RETURN_CODE is usually set to.
inline constexpr int kSkipReturnCode = 77;

/// Number of sections skipped so far in this process.
inline int& skipCount() {
  static int count = 0;
  return count;
}

/// Records one skipped section and says why on stdout.
inline void skip(const char* what, const char* reason) {
  std::printf("  [SKIP] %s (%s)\n", what, reason);
  ++skipCount();
}

/// True when this process may create file and directory symlinks. The probe
/// runs once: it creates and removes a link in the temp directory.
inline bool symlinksAvailable() {
  static const bool available = [] {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "litholattice_symlink_probe";
    if (ec) return false;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) return false;
    { std::FILE* f = std::fopen((dir / "target").string().c_str(), "wb"); if (f) std::fclose(f); }
    fs::create_symlink("target", dir / "file_link", ec);
    bool ok = !ec;
    if (ok) {
      fs::create_directory_symlink(dir, dir / "dir_link", ec);
      ok = !ec;
    }
    fs::remove_all(dir, ec);
    return ok;
  }();
  return available;
}

/// Reason string for a symlink skip, pointing at the fix.
inline const char* symlinkSkipReason() {
  return "this process may not create symlinks; on Windows enable Developer Mode "
         "or run from an elevated shell, see BUILDING.md";
}

}  // namespace litholattice::test_env
