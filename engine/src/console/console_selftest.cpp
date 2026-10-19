// console_selftest.cpp: headless checks for the console module.
#include "litholattice/console/console_selftest.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "litholattice/console/completer.h"
#include "litholattice/console/debug_console.h"
#include "litholattice/console/delimiter_pairing.h"
#include "litholattice/console/script_console.h"
#include "litholattice/core/log.h"
#include "litholattice/defrag/defrag.h"
#include "litholattice/mods/mod_interface.h"  // kPermConsole
#include "litholattice/mods/mod_permissions.h"
#include "litholattice/ui/text_edit.h"
#include "litholattice/ui/ui_root.h"

namespace litholattice::console {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] console: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

/// Captures registry output lines.
struct Capture {
  std::vector<std::string> lines;
  void install(ConsoleRegistry& r) {
    r.setOutput([this](const std::string& line) { lines.push_back(line); });
  }
  bool contains(const std::string& sub) const {
    for (const auto& l : lines)
      if (l.find(sub) != std::string::npos) return true;
    return false;
  }
  void clear() { lines.clear(); }
};

std::filesystem::path tempDir(const std::string& name) {
  auto dir = std::filesystem::temp_directory_path() / ("litholattice_console_" + name);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

ui::UiEvent tildeEvent() { return ui::UiEvent::text(U'~'); }
ui::UiEvent keyEvent(ui::Key k) { return ui::UiEvent::keyDown(k); }

}  // namespace

int runConsoleSelfTest() {
  Checker check;
  std::printf("console: engine-log tap, lithoscript, AngelScript exposure, debug console\n");

  // ---- engine-log tap: levels, order, last-N ----
  {
    engineLogTap().clear();
    logInfo("tap one");
    logWarn("tap two");
    logError("tap three");
    const auto snap = engineLogTap().snapshot();
    bool ok = snap.size() == 3 && snap[0].level == LogLevel::Info && snap[1].level == LogLevel::Warn &&
              snap[2].level == LogLevel::Error && snap[0].text == "tap one" &&
              snap[2].text == "tap three" && !snap[0].timestamp.empty() &&
              snap[0].seq + 1 == snap[1].seq && snap[1].seq + 1 == snap[2].seq;
    check(ok, "log tap: levels, text, timestamps and monotonic seqs in order");
    for (int i = 0; i < 600; ++i) logInfo("filler %d", i);
    const auto snap2 = engineLogTap().snapshot();
    ok = snap2.size() == LogTap::kCapacity && snap2.back().text == "filler 599" &&
         snap2.back().seq - snap2.front().seq + 1 == LogTap::kCapacity;
    check(ok, "log tap: keeps the last 512 records, drops the oldest");
    engineLogTap().clear();
    check(engineLogTap().size() == 0, "log tap: clear() empties it");
  }

  // ---- permission bit: canonical name table ----
  {
    using mods::permissionBitForName;
    using mods::permissionNameForBit;
    bool ok = permissionBitForName("console") == uint32_t(kPermConsole) &&
              std::string(permissionNameForBit(kPermConsole)) == "console" &&
              (mods::kAllPermissionBits & uint32_t(kPermConsole)) != 0;
    check(ok, "permissions: new kPermConsole bit named \"console\" in the canonical table");
  }

  // ---- tokenizer ----
  {
    bool ok = true;
    auto t = ConsoleRegistry::tokenize(R"(echo "a b" c)");
    ok = ok && t.size() == 1 && t[0].size() == 3 && t[0][0] == "echo" && t[0][1] == "a b" && t[0][2] == "c";
    t = ConsoleRegistry::tokenize(R"(echo a\;b "x\"y" z\\w)");
    ok = ok && t.size() == 1 && t[0].size() == 4 && t[0][1] == "a;b" && t[0][2] == "x\"y" && t[0][3] == "z\\w";
    t = ConsoleRegistry::tokenize("echo hi // a comment");
    ok = ok && t.size() == 1 && t[0].size() == 2;  // comment stripped
    t = ConsoleRegistry::tokenize(R"(echo "// not a comment")");
    ok = ok && t.size() == 1 && t[0].size() == 2 && t[0][1] == "// not a comment";
    t = ConsoleRegistry::tokenize("echo a;echo b;; echo c");
    ok = ok && t.size() == 3 && t[2][1] == "c";  // empty commands dropped
    check(ok, "tokenizer: quotes, escapes, // comments, ';' splitting");
  }

  // ---- tokenizer: /* */ block comments ----
  {
    bool ok = true;
    auto t = ConsoleRegistry::tokenize("echo a /* hidden */ b");
    ok = ok && t.size() == 1 && t[0].size() == 3 && t[0][0] == "echo" && t[0][1] == "a" &&
         t[0][2] == "b";  // block comment stripped mid-line
    t = ConsoleRegistry::tokenize("/**/echo a/**/");
    ok = ok && t.size() == 1 && t[0].size() == 2 && t[0][1] == "a";  // adjacent markers
    t = ConsoleRegistry::tokenize("echo a /* ; echo evil */ echo b");
    ok = ok && t.size() == 1 && t[0].size() == 4;  // ';' inside a block comment does not split
    t = ConsoleRegistry::tokenize("echo a /* // still comment */ b");
    ok = ok && t.size() == 1 && t[0].size() == 3 && t[0][2] == "b";  // // inside /* */ is text
    t = ConsoleRegistry::tokenize(R"(echo "/* not a comment */")");
    ok = ok && t.size() == 1 && t[0].size() == 2 &&
         t[0][1] == "/* not a comment */";  // /* inside quotes is literal
    t = ConsoleRegistry::tokenize(R"(echo "a */ b")");
    ok = ok && t.size() == 1 && t[0].size() == 2 && t[0][1] == "a */ b";  // */ in quotes literal
    t = ConsoleRegistry::tokenize(R"(echo a\/*b)");
    ok = ok && t.size() == 1 && t[0].size() == 2 && t[0][1] == "a/*b";  // escaped marker is literal
    t = ConsoleRegistry::tokenize("echo a /* x */ // y");
    ok = ok && t.size() == 1 && t[0].size() == 2;  // mixed /* */ and //
    t = ConsoleRegistry::tokenize("echo a // /* not a block");
    ok = ok && t.size() == 1 && t[0].size() == 2;  // /* inside a // comment is text
    t = ConsoleRegistry::tokenize("echo a /* // unterminated");
    ok = ok && t.empty();  // the // does not close the /*: unterminated -> empty
    check(ok, "tokenizer: /* */ stripped mid-line, ';' inside is inert, markers in "
              "strings/escapes//-comments are literal");

    // Unterminated /* is a clean tokenizer error, never a silent truncation.
    std::string err;
    t = ConsoleRegistry::tokenize("echo a /* oops", &err);
    ok = t.empty() && err == "unterminated block comment";
    err.clear();
    t = ConsoleRegistry::tokenize("echo ok /* fine */ done", &err);
    ok = ok && err.empty() && t.size() == 1 && t[0].size() == 3 && t[0][2] == "done";
    // The single-argument form keeps working: on error it yields the same
    // empty list (documented; the error is dropped).
    t = ConsoleRegistry::tokenize("echo a /* oops");
    ok = ok && t.empty();
    t = ConsoleRegistry::tokenize("echo fine /* closed */");
    ok = ok && t.size() == 1 && t[0].size() == 2;
    check(ok, "tokenizer: unterminated /* reports 'unterminated block comment' via the "
              "error overload; the one-argument form stays compatible");
  }

  // ---- extractComments ----
  {
    bool ok = true;
    std::string err;
    auto c = ConsoleRegistry::extractComments("// line one\n"
                                              "echo a /* block A */ b // trailing\n"
                                              "/* multi\n"
                                              "line */\n"
                                              "echo \"/* quoted */\"\n",
                                              &err);
    ok = ok && err.empty() && c.size() == 4 && c[0] == " line one" && c[1] == " block A " &&
         c[2] == " trailing" && c[3] == " multi\nline ";
    // Bodies come back in source order, without markers; the quoted /* */
    // yields nothing. CRLF: the trailing \r is not part of a // body.
    c = ConsoleRegistry::extractComments("echo hi // greeting\r\n", &err);
    ok = ok && err.empty() && c.size() == 1 && c[0] == " greeting";
    // No comments at all: empty, no error.
    c = ConsoleRegistry::extractComments("echo a; echo \"//x\"\n", &err);
    ok = ok && err.empty() && c.empty();
    // Unterminated /*: the error is set, and what was extracted so far is kept.
    err.clear();
    c = ConsoleRegistry::extractComments("/* a */ // b\n/* c", &err);
    ok = ok && err == "unterminated block comment" && c.size() == 2 && c[0] == " a " &&
         c[1] == " b";
    check(ok, "extractComments: // and /* */ bodies in source order without markers, "
              "multi-line bodies, string/escape protection, unterminated /* keeps "
              "what was extracted and reports the error");
  }

  // ---- command dispatch incl. unknown-command error ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    bool ok = r.registerCommand("add", "adds", [](ConsoleRegistry& rr, const ConsoleArgs& argv) {
                  long sum = 0;
                  for (size_t i = 1; i < argv.size(); ++i) sum += std::stol(argv[i]);
                  rr.print(std::to_string(sum));
                }, &err) &&
              err.empty();
    r.executeLine("add 2 3 4");
    ok = ok && cap.contains("9");
    r.executeLine("nope_nothing");
    ok = ok && cap.contains("Unknown command");
    ok = ok && !r.registerCommand("add", "dup", [](ConsoleRegistry&, const ConsoleArgs&) {}, &err) &&
         !err.empty();
    ok = ok && !r.registerCommand("BAD NAME", "x", [](ConsoleRegistry&, const ConsoleArgs&) {}, &err);
    check(ok, "commands: dispatch, unknown-command error, duplicate/invalid names refused");
  }

  // ---- aliases: definition, expansion, nesting, recursion cap ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    bool ok = r.setAlias("greet", "echo hi", &err);
    r.executeLine("greet");
    ok = ok && cap.contains("hi");
    r.setAlias("a", "b");
    r.setAlias("b", "echo deep");
    r.executeLine("a");
    ok = ok && cap.contains("deep");  // nested alias expansion
    r.setAlias("g", "echo");
    r.executeLine("g hello");
    ok = ok && cap.contains("hello");  // extra args appended Quake-style
    r.setAlias("loop", "loop");
    cap.clear();
    r.executeLine("loop");
    ok = ok && cap.contains("alias recursion too deep");  // HL1-style infinite-alias protection
    ok = ok && !r.setAlias("echo", "x", &err);             // cannot shadow a command
    check(ok, "aliases: define, expand, nest, append args, recursion cap, no shadowing");
  }

  // ---- cvars: get/set, type coercion, archived round-trip ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    bool ok = r.registerCVar("myint", CVarType::Int, "1", true, "an int", &err) &&
              r.registerCVar("myfloat", CVarType::Float, "0.5", true, "a float", &err) &&
              r.registerCVar("mybool", CVarType::Bool, "0", true, "a bool", &err) &&
              r.registerCVar("mystr", CVarType::String, "hi", false, "a string", &err) &&
              r.registerCVar("bad", CVarType::Int, "notanint", true, "x", &err) == false;
    r.executeLine("myint 42");
    ok = ok && r.findCVar("myint")->value == "42";
    r.executeLine("myint abc");  // bad value: error, old value kept
    ok = ok && cap.contains("not a int") && r.findCVar("myint")->value == "42";
    r.executeLine("mybool on");
    ok = ok && r.findCVar("mybool")->value == "1";
    r.executeLine("mybool OFF");
    ok = ok && r.findCVar("mybool")->value == "0";
    r.executeLine("myfloat 1.5");
    ok = ok && r.findCVar("myfloat")->value == "1.5";
    r.executeLine("mybool");  // get prints the value
    ok = ok && cap.contains("\"mybool\" = \"0\"");
    r.executeLine("nosuchcvar 1");
    ok = ok && cap.contains("Unknown command");  // unknown cvar is an unknown command
    check(ok, "cvars: get/set, int/float/bool coercion, bad values rejected");

    // archived round-trip through config save/load (saveArchived is sandboxed
    // to the config dir; archived cvars boot back via the sandboxed exec path)
    const auto dir = tempDir("cvars");
    const auto cfg = dir / "archived.lth";
    r.executeLine("myint 7");
    r.executeLine("mystr changed");  // not archived: must NOT survive
    err.clear();
    r.setConfigDir(dir);
    ok = r.saveArchived(cfg, &err) && err.empty();
    // saveArchived refuses to write outside the config dir (fail-closed)
    std::string err2;
    ok = ok && !r.saveArchived(dir.parent_path() / "escape.lth", &err2) && !err2.empty();
    ConsoleRegistry r2;
    r2.registerCVar("myint", CVarType::Int, "1", true, "", &err);
    r2.registerCVar("myfloat", CVarType::Float, "0.5", true, "", &err);
    r2.registerCVar("mybool", CVarType::Bool, "0", true, "", &err);
    r2.registerCVar("mystr", CVarType::String, "hi", false, "", &err);
    r2.setConfigDir(dir);
    ok = ok && r2.executeFile("archived.lth");
    ok = ok && r2.findCVar("myint")->value == "7" && r2.findCVar("myfloat")->value == "1.5" &&
         r2.findCVar("mybool")->value == "0" && r2.findCVar("mystr")->value == "hi";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    check(ok, "cvars: archived round-trip through config save/load (non-archived excluded)");
  }

  // ---- exec: sandboxed to the config dir; .lth is the script extension ----
  {
    const auto dir = tempDir("exec");
    {
      std::ofstream out(dir / "test.lth");
      out << "// a comment\nmyint 11\necho from_lth\n";
    }
    {
      std::ofstream out(dir / "old.cfg");
      out << "myint 12\necho from_legacy_cfg\n";
    }
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    r.registerCVar("myint", CVarType::Int, "0", true, "", &err);
    r.setConfigDir(dir);
    bool ok = r.executeFile("test.lth");
    ok = ok && cap.contains("from_lth") && r.findCVar("myint")->value == "11";
    // .cfg still executes as a legacy alias.
    ok = ok && r.executeFile("old.cfg");
    ok = ok && cap.contains("from_legacy_cfg") && r.findCVar("myint")->value == "12";
    cap.clear();
    ok = ok && !r.executeFile("../evil.lth") && cap.contains("escapes the config dir");
    ok = ok && !r.executeFile("/abs/path.lth") && cap.contains("escapes the config dir");
    ok = ok && !r.executeFile("evil.txt") && cap.contains("only .lth");
    ok = ok && !r.executeFile("missing.lth") && cap.contains("cannot open");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    check(ok, "exec: runs .lth from the config dir; .cfg works as a legacy alias; "
              "refuses .., absolute and non-script paths");
  }

  // ---- exec: block comments may span lines (stripped across the whole file) ----
  {
    const auto dir = tempDir("exec_block");
    {
      std::ofstream out(dir / "block.lth");
      out << "/* a block comment\n"
             "   spanning lines; echo evil\n"
             "   // still comment text\n"
             "*/\n"
             "myint 21\n"
             "echo /* inline */ from_block\n"
             "echo \"/* kept */\"\n";
    }
    {
      std::ofstream out(dir / "unterminated.lth");
      out << "myint 22\n/* never closed\n";
    }
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    r.registerCVar("myint", CVarType::Int, "0", true, "", &err);
    r.setConfigDir(dir);
    // The multi-line comment (including the `;` and `echo evil` inside it)
    // vanishes before line splitting; the rest executes.
    bool ok = r.executeFile("block.lth");
    ok = ok && r.findCVar("myint")->value == "21" && cap.contains("from_block") &&
         cap.contains("/* kept */") && !cap.contains("evil");
    // Unterminated block comment: the whole file is refused fail-closed.
    cap.clear();
    ok = ok && !r.executeFile("unterminated.lth") &&
         cap.contains("unterminated block comment") && r.findCVar("myint")->value == "21";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    check(ok, "exec: /* */ stripped across the whole file (multi-line works, ';' inside "
              "is inert, markers in strings kept); unterminated /* refuses the file");
  }

  // ---- hardening: exec-chain depth cap, mutual recursion, wait omission ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(true);  // debug builtins included: still no wait/delay
    std::string err;

    // `wait` exists now (JD 2026-10-11, hardened): `delay`/`sleep` must
    // NOT — one pacing command is enough, and it is the audited one.
    bool ok = r.hasCommand("wait") && !r.hasCommand("delay") && !r.hasCommand("sleep");
    check(ok, "hardening: wait exists; delay/sleep must not (single hardened pacing command)");

    // Mutual alias recursion (a -> b -> a) terminates at the recursion cap.
    r.setAlias("ma", "mb", &err);
    r.setAlias("mb", "ma", &err);
    cap.clear();
    r.executeLine("ma");
    ok = cap.contains("alias recursion too deep");
    check(ok, "hardening: mutual alias recursion (a->b->a) terminates at the cap");

    // Exec cycle across two files terminates at the exec-chain depth cap.
    const auto dir = tempDir("exec_cycle");
    {
      std::ofstream out(dir / "a.lth");
      out << "exec b.lth\n";
    }
    {
      std::ofstream out(dir / "b.lth");
      out << "exec a.lth\n";
    }
    r.setConfigDir(dir);
    cap.clear();
    r.executeFile("a.lth");  // must return; the cycle is cut by the depth cap
    ok = cap.contains("script nesting too deep");
    // The depth counter must have unwound back to zero (a later exec works).
    {
      std::ofstream out(dir / "fine.lth");
      out << "echo fine_after_cycle\n";
    }
    cap.clear();
    ok = ok && r.executeFile("fine.lth") && cap.contains("fine_after_cycle");

    // Exec nesting deeper than 8 is rejected: a 10-deep chain never reaches
    // the bottom file.
    {
      const int chain = 10;
      for (int i = 0; i < chain; ++i) {
        std::ofstream out(dir / ("f" + std::to_string(i) + ".lth"));
        if (i + 1 < chain)
          out << "exec f" + std::to_string(i + 1) + ".lth\n";
        else
          out << "echo bottom_reached\n";
      }
    }
    cap.clear();
    r.executeFile("f0.lth");
    ok = ok && cap.contains("script nesting too deep") && !cap.contains("bottom_reached");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    check(ok, "hardening: exec cycle a.lth<->b.lth terminates; nesting past 8 is "
              "rejected; depth counter unwinds after a cycle");
  }

#if defined(__linux__)
  // ---- exec refuses symlinks (TOCTOU hardening: O_NOFOLLOW) ----
  {
    const auto dir = tempDir("exec_symlink");
    {
      std::ofstream out(dir / "real.lth");
      out << "myint 99\n";
    }
    std::error_code ec;
    // The old resolve-then-open would FOLLOW this (it points inside the
    // config dir, so the containment check passes); O_NOFOLLOW refuses it.
    std::filesystem::create_symlink(dir / "real.lth", dir / "link.lth", ec);
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    std::string err;
    r.registerCVar("myint", CVarType::Int, "0", true, "", &err);
    r.setConfigDir(dir);
    bool ok = !ec;
    cap.clear();
    ok = ok && !r.executeFile("link.lth");  // refused: symlink
    ok = ok && cap.contains("symlink refused") && r.findCVar("myint")->value == "0";
    cap.clear();
    ok = ok && r.executeFile("real.lth");  // the real file still executes
    ok = ok && r.findCVar("myint")->value == "99";
    std::filesystem::remove_all(dir, ec);
    check(ok, "exec: symlinks refused (O_NOFOLLOW + fd re-validation); regular files unaffected");
  }
#endif

  // ---- wait: cooperative tick-based suspension, hardened ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;

    // wait N suspends the REST of the line for exactly N ticks.
    r.executeLine("echo before; wait 2; echo after");
    bool ok = cap.contains("before") && !cap.contains("after") && r.suspendedCount() == 1;
    r.tick();  // t+1: still waiting
    ok = ok && !cap.contains("after") && r.suspendedCount() == 1;
    r.tick();  // t+2: resumes
    ok = ok && cap.contains("after") && r.suspendedCount() == 0;
    check(ok, "wait: suspends the rest of the line for exactly N ticks, then resumes");

    // wait 0 is a documented no-op: no suspension. Bare `wait` = 1 tick.
    cap.clear();
    r.executeLine("echo a; wait 0; echo b");
    ok = cap.contains("a") && cap.contains("b") && r.suspendedCount() == 0;
    cap.clear();
    r.executeLine("echo c; wait; echo d");
    ok = ok && cap.contains("c") && !cap.contains("d") && r.suspendedCount() == 1;
    r.tick();
    ok = ok && cap.contains("d") && r.suspendedCount() == 0;
    // Negative / non-numeric are usage errors, never suspensions.
    cap.clear();
    r.executeLine("wait -3");
    r.executeLine("wait banana");
    ok = ok && cap.contains("usage: wait") && r.suspendedCount() == 0;
    check(ok, "wait: wait 0 is a no-op; bare wait = 1 tick; bad args are usage errors");

    // Clamp: above 512 ticks clamps with a note (graceful, never a hang).
    cap.clear();
    r.executeLine("echo x; wait 999999999; echo y");
    ok = cap.contains("clamped to 512") && !cap.contains("y");
    for (int i = 0; i < 511; ++i) r.tick();
    ok = ok && !cap.contains("y") && r.suspendedCount() == 1;
    r.tick();  // the 512th tick: resumes
    ok = ok && cap.contains("y") && r.suspendedCount() == 0;
    check(ok, "wait: values above 512 clamp to 512 with a note (wait 999999999 cannot hang)");

    // The registry stays live during a wait: other lines run immediately.
    cap.clear();
    r.executeLine("wait 512");
    r.executeLine("echo during");
    ok = cap.contains("during") && r.suspendedCount() == 1;
    for (int i = 0; i < 512; ++i) r.tick();
    ok = ok && r.suspendedCount() == 0;
    check(ok, "wait: other commands run during a wait (the engine is never blocked)");

    // wait 0 in a self-loop still hits the alias cap (no suspension to exploit).
    ok = r.setAlias("zl", "wait 0; zl", &err);
    cap.clear();
    r.executeLine("zl");
    ok = ok && cap.contains("alias recursion too deep") && r.suspendedCount() == 0;
    check(ok, "wait: wait-0 self-loop terminates at the alias cap");

    // wait/alias ping-pong terminates: the alias cap survives suspension.
    ok = r.setAlias("wa", "wait 1; wb", &err) && r.setAlias("wb", "wait 1; wa", &err);
    cap.clear();
    r.executeLine("wa");
    for (int i = 0; i < 64; ++i) r.tick();
    ok = ok && cap.contains("alias recursion too deep") && r.suspendedCount() == 0;
    check(ok, "wait: wait/alias ping-pong terminates (alias cap preserved across suspension)");

    // wait/alias self-loop: `alias ws "wait 1; ws"` — same, via the cap.
    ok = r.setAlias("ws", "wait 1; ws", &err);
    cap.clear();
    r.executeLine("ws");
    for (int i = 0; i < 64; ++i) r.tick();
    ok = ok && cap.contains("alias recursion too deep") && r.suspendedCount() == 0;
    check(ok, "wait: wait/alias self-loop terminates at the alias cap");
  }

  // ---- wait: exec cycle with waits terminates at the total-wait budget ----
  {
    const auto dir = tempDir("wait_exec_cycle");
    {
      std::ofstream out(dir / "wa.lth");
      out << "wait 1\nexec wb.lth\n";
    }
    {
      std::ofstream out(dir / "wb.lth");
      out << "wait 1\nexec wa.lth\n";
    }
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    r.setConfigDir(dir);
    cap.clear();
    const bool started = r.executeFile("wa.lth");  // accepted (suspension is not failure)
    bool ok = started && r.suspendedCount() == 1;
    for (int i = 0; i < 5000; ++i) r.tick();  // budget is 4096: must terminate fail-closed
    ok = ok && cap.contains("total wait budget exceeded") && r.suspendedCount() == 0;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    check(ok, "wait: exec cycle with waits terminates at the total-wait budget (fail-closed)");
  }

  // ---- wait: step budget bounds per-invocation CPU ----
  {
    const auto dir = tempDir("wait_steps");
    {
      std::ofstream out(dir / "many.lth");
      for (int i = 0; i < 20000; ++i) out << "echo line\n";
    }
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    r.setConfigDir(dir);
    cap.clear();
    r.executeFile("many.lth");
    const bool ok = cap.contains("step budget exceeded") && r.suspendedCount() == 0;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    check(ok, "wait: step budget terminates runaway invocations (fail-closed)");
  }

  // ---- toggle / incrementvar / ++/-- : HL1 language completion ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    bool ok = r.registerCVar("tb", CVarType::Bool, "0", false, "", &err) &&
              r.registerCVar("ti", CVarType::Int, "0", false, "", &err) &&
              r.registerCVar("tf", CVarType::Float, "1.5", false, "", &err) &&
              r.registerCVar("ts", CVarType::String, "x", false, "", &err);
    // toggle flips bool and int 0/1
    r.executeLine("toggle tb");
    ok = ok && r.findCVar("tb")->value == "1";
    r.executeLine("toggle tb");
    ok = ok && r.findCVar("tb")->value == "0";
    r.executeLine("toggle ti");
    ok = ok && r.findCVar("ti")->value == "1";
    // toggle cycles a value list; unlisted current starts at vals[0]
    r.executeLine("toggle ti 0 5 10");  // "1" unlisted -> "0"
    ok = ok && r.findCVar("ti")->value == "0";
    r.executeLine("toggle ti 0 5 10");  // "0" -> "5"
    ok = ok && r.findCVar("ti")->value == "5";
    r.executeLine("toggle ti 0 5 10");  // "5" -> "10"
    ok = ok && r.findCVar("ti")->value == "10";
    r.executeLine("toggle ti 0 5 10");  // "10" -> "0" (wrap)
    ok = ok && r.findCVar("ti")->value == "0";
    cap.clear();
    r.executeLine("toggle tf");  // float without values -> error
    ok = ok && cap.contains("not a bool/int");
    r.executeLine("toggle ti 0 banana");
    ok = ok && cap.contains("not a int");
    r.executeLine("toggle nope");
    ok = ok && cap.contains("unknown cvar");
    check(ok, "toggle: flip bool/int, cycle value lists with wrap, errors");

    // incrementvar: add, clamp at min/max, float
    r.executeLine("incrementvar ti 0 10 3");
    ok = ok && r.findCVar("ti")->value == "3";
    r.executeLine("incrementvar ti 0 10 9");  // 3+9=12 -> clamp 10
    ok = ok && r.findCVar("ti")->value == "10";
    r.executeLine("incrementvar ti 0 10 -25");  // 10-25 -> clamp 0
    ok = ok && r.findCVar("ti")->value == "0";
    r.executeLine("incrementvar tf 0 2 0.5");  // 1.5+0.5 -> 2
    ok = ok && r.findCVar("tf")->value == "2";
    cap.clear();
    r.executeLine("incrementvar ts 0 1 1");
    ok = ok && cap.contains("not an int/float");
    r.executeLine("incrementvar ti 5 0 1");
    ok = ok && cap.contains("min > max");
    r.executeLine("incrementvar nope 0 1 1");
    ok = ok && cap.contains("unknown cvar");
    check(ok, "incrementvar: add, clamp at min/max, float, errors");

    // ++/-- shorthand
    r.executeLine("ti++");
    ok = ok && r.findCVar("ti")->value == "1";
    r.executeLine("ti--");
    ok = ok && r.findCVar("ti")->value == "0";
    r.executeLine("tf++");
    ok = ok && r.findCVar("tf")->value == "3";
    r.executeLine("tf--");
    ok = ok && r.findCVar("tf")->value == "2";
    cap.clear();
    r.executeLine("ts++");
    ok = ok && cap.contains("not an int/float");
    r.executeLine("nope++");
    ok = ok && cap.contains("Unknown command");
    r.executeLine("ti++ 5");
    ok = ok && cap.contains("usage:");
    check(ok, "++/--: int and float cvars, errors on string/unknown/args");

    // F-L6: non-finite deltas fail closed — the cvar is left untouched
    // (the old code silently zeroed an int cvar on a NaN delta).
    r.executeLine("incrementvar ti 0 10 5");
    ok = ok && r.findCVar("ti")->value == "5";
    cap.clear();
    r.executeLine("incrementvar ti 0 10 nan");
    ok = ok && cap.contains("incrementvar: failed") && r.findCVar("ti")->value == "5";
    r.executeLine("incrementvar tf 0 2 inf");
    ok = ok && cap.contains("incrementvar: failed") && r.findCVar("tf")->value == "2";
    check(ok, "incrementvar: nan/inf delta refused, cvar unchanged (F-L6)");

    // F-L5: an unterminated block comment on an interactive line prints the
    // error and runs nothing; a terminated comment still runs the command.
    cap.clear();
    r.executeLine("echo hi /* oops");
    ok = ok && cap.contains("unterminated block comment") && !cap.contains("hi");
    cap.clear();
    r.executeLine("echo fine /* ok */");
    ok = ok && cap.contains("fine");
    check(ok, "executeLine: unterminated /* prints an error, runs nothing (F-L5)");
  }

  // ---- find / unbindall ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    r.registerCVar("my_volume", CVarType::Float, "0.5", false, "", &err);
    r.setAlias("myalias", "echo hi", &err);
    cap.clear();
    r.executeLine("find vol");
    bool ok = cap.contains("cvar: \"my_volume\"");
    r.executeLine("find ECHO");  // case-insensitive
    ok = ok && cap.contains("command: echo");
    r.executeLine("find myalias");
    ok = ok && cap.contains("alias: myalias");
    cap.clear();
    r.executeLine("find zzz_no_match");
    ok = ok && cap.contains("no matches");
    check(ok, "find: matches commands, cvars, aliases (case-insensitive)");

    ok = r.setKeyBind("F5", "echo hi", &err) && r.setKeyBind("F6", "echo yo", &err);
    r.executeLine("unbindall");
    ok = ok && r.keyBinds().empty();
    check(ok, "unbindall: clears all key binds");
  }

  // ---- bind dispatch: console owns the table, the pump dispatches ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    bool ok = r.setKeyBind("F5", "echo bound_fired", &err);
    ok = ok && r.setAlias("bhi", "echo alias_in_bind", &err);
    ok = ok && r.setKeyBind("F6", "bhi; echo seq2", &err);
    ok = ok && r.setKeyBind("F7", "echo w1; wait 2; echo w2", &err);

    DeviceState ds;
    using T = InputEvent::Type;
    uint64_t seq = 0;
    auto press = [&](Key k) {
      ds.beginFrame();
      ds.apply(InputEvent::key(T::KeyDown, k, seq++));
    };
    auto release = [&](Key k) {
      ds.beginFrame();
      ds.apply(InputEvent::key(T::KeyUp, k, seq++));
    };

    press(Key::F5);
    r.dispatchKeyBinds(ds, false);
    ok = ok && cap.contains("bound_fired");
    // text capture suppresses dispatch (same rule as the tilde key)
    cap.clear();
    release(Key::F5);
    press(Key::F5);
    r.dispatchKeyBinds(ds, true);
    ok = ok && !cap.contains("bound_fired");
    // aliases and ';' sequences work in bound commands
    cap.clear();
    release(Key::F5);
    press(Key::F6);
    r.dispatchKeyBinds(ds, false);
    ok = ok && cap.contains("alias_in_bind") && cap.contains("seq2");
    // wait inside a bound command behaves (cooperative, budgeted)
    cap.clear();
    release(Key::F6);
    press(Key::F7);
    r.dispatchKeyBinds(ds, false);
    ok = ok && cap.contains("w1") && !cap.contains("w2") && r.suspendedCount() == 1;
    r.tick();
    r.tick();
    ok = ok && cap.contains("w2") && r.suspendedCount() == 0;
    // empty table dispatches nothing, no crash
    r.clearKeyBinds();
    r.dispatchKeyBinds(ds, false);
    check(ok, "bind dispatch: key press runs the command; aliases/sequences/wait work; "
              "text capture suppresses");
  }

  // ---- cvar flags: readonly / cheat / replicated (Decision 8) ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    std::string err;
    CVarFlags ro;
    ro.readOnly = true;
    CVarFlags ch;
    ch.cheat = true;
    CVarFlags rep;
    rep.replicated = true;
    bool ok = r.registerCVar("ro_cv", CVarType::Int, "1", false, "", ro, &err) &&
              r.registerCVar("ch_cv", CVarType::Int, "1", false, "", ch, &err) &&
              r.registerCVar("rep_cv", CVarType::Int, "1", false, "", rep, &err);
    // read-only: set refused, value kept
    ok = ok && !r.setCVar("ro_cv", "2", &err) && !err.empty() &&
         r.findCVar("ro_cv")->value == "1";
    // cheat: refused by default (fail-closed), allowed when the context opts in
    ok = ok && !r.cheatsAllowed();
    ok = ok && !r.setCVar("ch_cv", "2", &err) && r.findCVar("ch_cv")->value == "1";
    r.setCheatsAllowed(true);
    ok = ok && r.setCVar("ch_cv", "2", &err) && r.findCVar("ch_cv")->value == "2";
    // replicated is metadata for the netcode: settable, shown in cvarlist
    ok = ok && r.setCVar("rep_cv", "5", &err);
    r.registerBuiltins(false);
    cap.clear();
    r.executeLine("cvarlist");
    ok = ok && cap.contains("readonly") && cap.contains("cheat") && cap.contains("replicated");
    check(ok, "cvar flags: readonly refused, cheat gated on context, replicated shown");
  }

  // ---- onChange hook ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    bool ok = r.registerCVar("hooked", CVarType::Int, "0", false, "", &err);
    int calls = 0;
    std::string seen;
    ok = ok && r.setCVarChangeFn("hooked", [&](ConsoleRegistry&, const CVar& cv) {
             ++calls;
             seen = cv.value;
           });
    r.executeLine("hooked 42");
    ok = ok && calls == 1 && seen == "42";
    r.executeLine("toggle hooked");  // fires through toggle too
    ok = ok && calls == 2 && seen == "0";
    ok = ok && !r.setCVarChangeFn("nope", [&](ConsoleRegistry&, const CVar&) {});
    check(ok, "onChange: fires after successful set (incl. via toggle); false for unknown cvar");
  }

  // ---- end-to-end: the debug console input path drives lithoscript ----
  {
    ui::UiRoot ui;
    ConsoleRegistry r;
    r.registerBuiltins(false);
    std::string err;
    r.registerCVar("e2e_int", CVarType::Int, "0", true, "", &err);
    DebugConsole con(r);
    con.attach(ui);
    con.open();
    // Command, cvar set + read, and alias expansion — all through the console
    // widget's submit path (onSubmit -> executeLine), not the registry API.
    con.submitForTest("echo e2e_cmd");
    con.submitForTest("e2e_int 42");
    con.submitForTest("alias e2e_hi \"echo e2e_alias\"");
    con.submitForTest("e2e_hi");
    bool ok = r.findCVar("e2e_int")->value == "42";
    ok = ok && con.historySize() == 4;  // all four lines went through history
    // "] " echoes (4) + command output (e2e_cmd) + alias expansion output
    // (e2e_alias): cvar set and alias define print nothing.
    ok = ok && con.logLineCount() >= 6;
    check(ok, "lithoscript end-to-end: command, cvar set/read and alias expansion "
              "all reachable from the debug console input path");
    con.close();
  }

  // ---- ';' sequences execute in order ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    r.executeLine("echo one; echo two;echo three");
    const bool ok = cap.lines.size() == 3 && cap.lines[0] == "one" && cap.lines[1] == "two" &&
                    cap.lines[2] == "three";
    check(ok, "';' sequences execute in order");
  }

  // ---- key-bind registry seam ----
  {
    ConsoleRegistry r;
    std::string err;
    bool ok = r.setKeyBind("F5", "echo hi", &err);
    ok = ok && !r.setKeyBind("NoSuchKey", "echo x", &err) && !err.empty();
    ok = ok && r.keyBinds().count("F5") == 1 && r.removeKeyBind("F5") && r.keyBinds().empty();
    check(ok, "bind: key names validated (registry seam; dispatch is game follow-up)");
  }

  // ---- key binds stamp the binder's authority (anti-laundering) ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    r.registerCommand(
        "dbg_cheatdrop", "test-only cheat-gated command",
        [](ConsoleRegistry& rr, const ConsoleArgs&) { rr.print("CHEAT_FIRED"); },
        CommandFlags{true}, &err);
    r.setCheatsAllowed(true);  // the player context allows cheats...
    DeviceState ds;
    auto press = [&](Key k, uint64_t seq) {
      ds.beginFrame();
      ds.apply(InputEvent::key(InputEvent::Type::KeyDown, k, seq));
    };
    // ...but a bind planted under a restricted mask must not gain cheat
    // authority at dispatch (the laundering the stamp closes).
    r.setCallerPermissions(0);  // restricted: cheat authority revoked
    bool ok = r.setKeyBind("F5", "dbg_cheatdrop", &err);
    r.setCallerPermissions(kCallerFullAuthority);
    cap.clear();
    press(Key::F5, 0);
    r.dispatchKeyBinds(ds, false);
    ok = ok && !cap.contains("CHEAT_FIRED") &&
         cap.contains("command 'dbg_cheatdrop' requires cheats (not allowed in this context)");
    // A bind planted with full authority fires normally (no behavior change
    // for player-typed binds).
    cap.clear();
    ok = ok && r.setKeyBind("F6", "dbg_cheatdrop", &err);
    press(Key::F6, 1);
    r.dispatchKeyBinds(ds, false);
    ok = ok && cap.contains("CHEAT_FIRED");
    check(ok, "bind: binder's caller mask is stamped; restricted-planted binds cannot launder "
              "cheat authority to dispatch time");
  }

  // ---- aliases stamp the definer's authority (anti-laundering, App. F H1) ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    std::string err;
    r.registerCommand(
        "dbg_cheatdrop", "test-only cheat-gated command",
        [](ConsoleRegistry& rr, const ConsoleArgs&) { rr.print("CHEAT_FIRED"); },
        CommandFlags{true}, &err);
    r.registerCommand(
        "dbg_sessionact", "test-only session-tier command",
        [](ConsoleRegistry& rr, const ConsoleArgs&) { rr.print("SESSION_FIRED"); },
        CommandFlags{false, true}, &err);
    r.setCheatsAllowed(true);  // the player context allows cheats...

    // The Appendix F H1 repro: a restricted caller defines the alias; a
    // full-authority caller invokes it later. The body must run with the
    // DEFINER's mask, so the cheat command is refused.
    r.setCallerPermissions(0);  // restricted (MCP agent / mod)
    r.executeLine("alias launder \"dbg_cheatdrop\"");
    r.setCallerPermissions(kCallerFullAuthority);
    cap.clear();
    r.executeLine("launder");
    bool ok = !cap.contains("CHEAT_FIRED") &&
              cap.contains("command 'dbg_cheatdrop' requires cheats (not allowed in this context)");
    check(ok, "alias: restricted-defined alias invoked at full authority cannot launder "
              "cheat authority (definer's mask re-installed at expansion)");

    // A full-authority player's own alias keeps working (no behavior change).
    cap.clear();
    r.executeLine("alias goodcheat \"dbg_cheatdrop\"");
    r.executeLine("goodcheat");
    ok = cap.contains("CHEAT_FIRED");
    check(ok, "alias: full-authority-defined alias still fires its cheat command");

    // Composition: a full-authority alias whose body invokes a
    // restricted-defined alias inherits the restriction (most restrictive
    // applicable mask wins — bitwise AND down the expansion chain).
    cap.clear();
    r.executeLine("alias wrapper \"launder\"");
    r.executeLine("wrapper");
    ok = !cap.contains("CHEAT_FIRED") &&
         cap.contains("command 'dbg_cheatdrop' requires cheats");
    check(ok, "alias: nested expansion composes masks (outer full + inner restricted = restricted)");

    // Composition across a `wait`: the continuation of a restricted-defined
    // alias resumes under the composed definer mask, not the invoker's.
    r.setCallerPermissions(0);
    r.executeLine("alias waitlaunder \"wait 1; dbg_cheatdrop\"");
    r.setCallerPermissions(kCallerFullAuthority);
    cap.clear();
    r.executeLine("waitlaunder");
    r.tick();
    r.tick();
    ok = !cap.contains("CHEAT_FIRED") &&
         cap.contains("command 'dbg_cheatdrop' requires cheats");
    check(ok, "alias: wait-suspended body resumes under the definer's mask (no laundering via wait)");

    // Session tier: restricted masks cannot run session commands even
    // though they are not cheat-flagged; full authority can, cheats or not.
    r.setCallerPermissions(0);
    cap.clear();
    r.executeLine("dbg_sessionact");
    ok = !cap.contains("SESSION_FIRED") &&
         cap.contains("command 'dbg_sessionact' requires session authority (not allowed in this context)");
    r.setCallerPermissions(kCallerFullAuthority);
    cap.clear();
    r.executeLine("dbg_sessionact");
    ok = ok && cap.contains("SESSION_FIRED");
    check(ok, "session tier: restricted mask refused session command; full authority runs it");

    // debug.defrag itself is session-tier now (Appendix F M6).
    r.setCallerPermissions(0);
    cap.clear();
    r.executeLine("debug.defrag status");
    ok = cap.contains("command 'debug.defrag' requires session authority");
    check(ok, "session tier: debug.defrag refused for restricted callers");
  }

  // ---- debug-build-only commands ----
  {
    ConsoleRegistry rDbg, rRel;
    rDbg.registerBuiltins(true);
    rRel.registerBuiltins(false);
    check(rDbg.hasCommand("dbg_logtap") && rDbg.hasCommand("dbg_registry") &&
              !rRel.hasCommand("dbg_logtap"),
          "debug builds register the extra verbose commands; release builds do not");
  }

  // ---- AngelScript exposure, permission-gated ----
  {
    registerConsoleScriptBindings();
    bool ok = ScriptBindingAllowlist::isAllowlisted("void consoleCmd(const string &in)") &&
              ScriptBindingAllowlist::requiredPermissions("string consoleCvar(const string &in)") ==
                  uint32_t(kPermConsole);
    check(ok, "AngelScript: console decls on the allowlist, gated on kPermConsole");

    ConsoleRegistry r;
    Capture cap;  // (kept off: the console widget owns the sink in the game)
    r.registerBuiltins(false);
    std::string err;
    r.registerCVar("myint", CVarType::Int, "0", true, "", &err);

    std::vector<std::string> seen;
    r.setOutput([&seen](const std::string& line) { seen.push_back(line); });

    ScriptEngine se;
    se.init();
    se.setModPermissions(uint32_t(kPermConsole));
    auto ctx = bindConsoleToScripts(se, r, "testmod");
    ok = ctx != nullptr;
    ok = ok && se.loadSource("consoletest",
                             "void onLoad() {\n"
                             "  consoleCmd(\"echo from_script\");\n"
                             "  consoleSetCvar(\"myint\", \"99\");\n"
                             "  string v = consoleCvar(\"myint\");\n"
                             "  consoleCmd(\"echo got_\" + v);\n"
                             "}\n") &&
         se.callHook("void onLoad()");
    ok = ok && r.findCVar("myint")->value == "99";
    bool sawScript = false, sawCvar = false;
    for (const auto& l : seen) {
      if (l == "from_script") sawScript = true;
      if (l == "got_99") sawCvar = true;
    }
    ok = ok && sawScript && sawCvar;
    check(ok, "AngelScript: script calls a command and reads/writes a cvar");

    // Without kPermConsole the bindings are refused.
    ScriptEngine seDenied;
    seDenied.init();
    seDenied.setModPermissions(0);
    auto deniedCtx = bindConsoleToScripts(seDenied, r, "evilmod");
    check(deniedCtx == nullptr, "AngelScript: console bindings refused without kPermConsole");
  }

  // ---- consoleCmd runs with the mod's mask, not ambient authority ----
  {
    registerConsoleScriptBindings();  // idempotent
    ConsoleRegistry r;
    r.registerBuiltins(false);
    std::string err;
    r.registerCommand(
        "dbg_cheatdrop", "test-only cheat-gated command",
        [](ConsoleRegistry& rr, const ConsoleArgs&) { rr.print("CHEAT_FIRED"); },
        CommandFlags{true}, &err);
    // The game enables cheats for the local console: ambient authority would
    // let any mod's consoleCmd run cheat commands. The binding must install
    // the mod's own (restricted) mask instead.
    r.setCheatsAllowed(true);
    std::vector<std::string> seen;
    r.setOutput([&seen](const std::string& line) { seen.push_back(line); });

    ScriptEngine se;
    se.init();
    se.setModPermissions(uint32_t(kPermConsole));  // console access, no cheat grant
    auto ctx = bindConsoleToScripts(se, r, "testmod");
    bool ok = ctx != nullptr;
    ok = ok && se.loadSource("ambienttest",
                             "void onLoad() {\n"
                             "  consoleCmd(\"dbg_cheatdrop\");\n"  // must be refused
                             "  consoleCmd(\"echo ambient_ok\");\n"  // must work
                             "}\n") &&
         se.callHook("void onLoad()");
    bool sawOk = false, sawCheat = false;
    for (const auto& l : seen) {
      if (l == "ambient_ok") sawOk = true;
      if (l == "CHEAT_FIRED") sawCheat = true;
    }
    ok = ok && sawOk && !sawCheat;
    check(ok, "AngelScript: consoleCmd installs the mod's mask (cheat commands refused even though "
              "the registry allows cheats)");
  }

  // ---- DebugConsole widget: toggle, tilde, confinement, history ----
  {
    ui::UiRoot ui;
    ConsoleRegistry r;
    DebugConsole con(r);
    con.attach(ui);
    bool ok = !con.isOpen();

    // Tilde with nothing focused: opens.
    ok = ok && con.handleTildeKey(ui, tildeEvent());
    ok = ok && con.isOpen();
    con.close();

    // Tilde while open closes it (sleeps keep each press outside the 200 ms
    // key-repeat debounce window).
    con.open();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ok = ok && con.handleTildeKey(ui, tildeEvent()) && !con.isOpen();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ok = ok && con.handleTildeKey(ui, tildeEvent()) && con.isOpen();
    // An immediate second tilde is swallowed by the debounce: consumed, no toggle.
    ok = ok && con.handleTildeKey(ui, tildeEvent()) && con.isOpen();
    con.close();

    // Tilde while a text field has focus: suppressed (typing "~" keeps working),
    // even right after a toggle — the confinement check runs before the debounce.
    ui::TextBox field;
    ui.setFocus(&field);
    ok = ok && ui.wantsKeyboard();
    ok = ok && !con.handleTildeKey(ui, tildeEvent()) && !con.isOpen();
    ui.setFocus(nullptr);

    // History: Up/Down walk submitted lines; Escape closes.
    con.open();
    con.submitForTest("echo one");
    con.submitForTest("echo two");
    ok = ok && con.historySize() == 2;
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Up)) && con.inputText() == "echo two";
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Up)) && con.inputText() == "echo one";
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Down)) && con.inputText() == "echo two";
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Down)) && con.inputText().empty();
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Escape)) && !con.isOpen();
    check(ok, "console widget: toggle, tilde suppressed while a field has focus, history, escape");
  }

  // ---- console displays engine-log tap records ----
  {
    engineLogTap().clear();
    ui::UiRoot ui;
    ConsoleRegistry r;
    DebugConsole con(r);
    con.attach(ui);
    con.open();
    logWarn("hello from the tap");
    logError("and an error");
    con.update();
    const bool ok = con.logLineCount() >= 2;
    check(ok, "console: engine log tap records appear in the log view");
    con.close();
  }

  // ---- query: no backend, bounds mandatory, strict parsing, region cap ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    r.executeLine("query blocks 1 0 0 0 1 1 1");
    const bool noBackend = cap.contains("no world backend registered");
    check(noBackend, "query: clean message when no world backend is registered");
  }
  {
    // Fake backend: deterministic hits for blockId 7, count 5, two entities.
    struct FakeQuery : IWorldQuery {
      WorldQueryResult queryBlocks(uint32_t id, int, int, int, int, int, int,
                                   std::string*) override {
        WorldQueryResult r;
        r.cellsScanned = 8;
        if (id == 7) {
          r.blocks = {{1, 0, 0, 7}, {0, 1, 0, 7}, {0, 0, 1, 7}};
          r.totalMatches = 3;
        }
        return r;
      }
      WorldQueryResult queryCount(uint32_t id, int, int, int, int, int, int,
                                  std::string*) override {
        WorldQueryResult r;
        r.cellsScanned = 8;
        r.totalMatches = id == 7 ? 5 : 0;
        return r;
      }
      WorldQueryResult queryEntities(const std::string& type, double, double, double, double,
                                     std::string* err) override {
        WorldQueryResult r;
        if (type != "*" && type != "item") {
          if (err) *err = "unknown entity type '" + type + "'";
          return r;
        }
        r.entities = {{3, 1, "item", 1.0, 2.0, 3.0}, {9, 1, "item", 4.0, 5.0, 6.0}};
        r.totalMatches = 2;
        return r;
      }
    } fake;

    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    r.setWorldQuery(&fake);

    r.executeLine("query blocks 7 0 0 0");  // missing bounds: fail-closed
    const bool needBounds = cap.contains("all six bounds required");
    cap.clear();
    r.executeLine("query blocks 7 0 0 0 x 1 1");  // non-numeric: fail-closed, never 0
    const bool badNum = cap.contains("not an integer");
    cap.clear();
    r.executeLine("query blocks 7 0 0 0 256 128 128");  // 257*129*129 > 2^22
    const bool tooLarge = cap.contains("region too large");
    cap.clear();
    r.executeLine("query bogus 7 0 0 0 1 1 1");
    const bool badSub = cap.contains("usage: query blocks|count");
    check(needBounds && badNum && tooLarge && badSub,
          "query: bounds mandatory, strict numbers, region cap, subcommand validation");

    cap.clear();
    r.executeLine("query blocks 7 0 0 0 1 1 1");
    // Fake returns hits in backend order; the command preserves it verbatim.
    const bool hits = cap.contains("3 matches in 8 cells") && cap.contains("(1, 0, 0)") &&
                      cap.contains("(0, 1, 0)") && cap.contains("(0, 0, 1)");
    cap.clear();
    r.executeLine("query count 7 0 0 0 1 1 1");
    const bool count = cap.contains("5 matches in 8 cells");
    cap.clear();
    r.executeLine("query entities item 0 0 0 16");
    const bool ents = cap.contains("2 matches") && cap.contains("#3 item") && cap.contains("#9 item");
    cap.clear();
    r.executeLine("query entities bogus 0 0 0 16");
    const bool badType = cap.contains("unknown entity type");
    cap.clear();
    r.executeLine("query entities * 0 0 0 -5");  // bad radius
    const bool badRadius = cap.contains("radius must be positive");
    check(hits && count && ents && badType && badRadius,
          "query: blocks/count/entities round-trip through a backend, errors surfaced");
    r.setWorldQuery(nullptr);
  }

  // ---- cheat-gated commands (Decision 8) ----
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    bool ran = false;
    CommandFlags cheat;
    cheat.cheat = true;
    r.registerCommand("boom", "test cheat command",
                      [&](ConsoleRegistry&, const ConsoleArgs&) { ran = true; }, cheat);
    r.executeLine("boom");  // cheats not allowed here: refused, fn never runs
    const bool refused = !ran && cap.contains("requires cheats");
    r.setCheatsAllowed(true);
    r.executeLine("boom");
    check(refused && ran, "query: cheat-gated commands refuse without context cheats, run with them");
  }

  // ---- caller permission mask (Phase 4 MCP seam, 2026-10-11) ----
  // "Agent scripts execute with the AGENT's permissions, never the
  // player's": installing a restricted caller mask revokes cheat authority
  // even when the player's context allows cheats. Default (mask never
  // installed) preserves the old behavior exactly.
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    check(r.callerPermissions() == kCallerFullAuthority,
          "caller: default mask is full authority (legacy behavior)");
    bool ran = false;
    CommandFlags cheat;
    cheat.cheat = true;
    r.registerCommand("agent_boom", "cheat-gated probe",
                      [&](ConsoleRegistry&, const ConsoleArgs&) { ran = true; }, cheat);
    std::string err;
    CVarFlags cvf;
    cvf.cheat = true;
    r.registerCVar("agent_cheatcvar", CVarType::Int, "0", false, "cheat cvar probe", cvf, &err);
    r.setCheatsAllowed(true);  // the PLAYER's context allows cheats...
    r.executeLine("agent_boom");
    check(ran, "caller: full mask + context cheats -> cheat command runs");
    r.setCVar("agent_cheatcvar", "1", &err);
    check(err.empty(), "caller: full mask + context cheats -> cheat cvar sets");
    // ...but a restricted agent mask revokes cheat authority anyway.
    const uint32_t agentMask = static_cast<uint32_t>(kPermConsole);  // the MCP agent's grant
    r.setCallerPermissions(agentMask);
    check(r.callerPermissions() == agentMask, "caller: installed mask reads back");
    ran = false;
    cap.clear();
    r.executeLine("agent_boom");
    const bool cmdRefused = !ran && cap.contains("requires cheats");
    err.clear();
    const bool cvarRefused = !r.setCVar("agent_cheatcvar", "2", &err) &&
                             err.find("requires cheats") != std::string::npos;
    check(cmdRefused && cvarRefused,
          "caller: restricted mask revokes cheats despite context cheatsAllowed");
    // A `wait`-suspended line resumes under the mask it started with.
    ran = false;
    cap.clear();
    r.executeLine("wait 1; agent_boom");
    check(r.suspendedCount() == 1, "caller: wait suspended the agent line");
    r.setCallerPermissions(kCallerFullAuthority);  // context changes under it...
    r.tick();                                      // ...but the resume keeps the agent mask
    check(!ran && cap.contains("requires cheats"),
          "caller: suspended script resumes under its original restricted mask");
    r.setCallerPermissions(kCallerFullAuthority);
    ran = false;
    cap.clear();
    r.executeLine("agent_boom");
    check(ran, "caller: restoring full authority restores cheat commands");
  }

  // debug.defrag wiring: service install, command surface, per-tick pump.
  {
    ConsoleRegistry r;
    Capture cap;
    cap.install(r);
    r.registerBuiltins(false);
    r.executeLine("debug.defrag");
    check(cap.contains("no defrag service registered"),
          "defrag: command reports missing service fail-closed");

    defrag::DefragService svc;
    r.setDefragService(&svc);
    cap.clear();
    r.executeLine("debug.defrag");
    check(cap.contains("idle"), "defrag: status idle with no session");

    defrag::RecordStore store;
    const std::string payload(128, 'q');
    for (uint64_t i = 1; i <= 40; ++i)
      store.put(i, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    for (uint64_t i = 1; i <= 40; i += 2) store.erase(i);
    check(store.deadBytes() > 0, "defrag: fixture fragmented");
    const auto fragPath =
        std::filesystem::temp_directory_path() / "litho_console_defrag_test";
    check(svc.registerStore("fragment", store, fragPath),
          "defrag: register fragment store ok");
    cap.clear();
    r.executeLine("debug.defrag fragment");
    check(cap.contains("started on 'fragment'"), "defrag: command starts session");
    for (int i = 0; i < 10000 && svc.running(); ++i) r.tick();  // pumped via tick()
    check(!svc.running() && svc.phase() == defrag::DefragService::Phase::Done,
          "defrag: session completes through registry tick()");
    check(store.deadBytes() == 0, "defrag: dead space reclaimed via tick pump");
    bool intact = true;
    for (uint64_t i = 2; i <= 40; i += 2) {
      auto g = store.get(i);
      if (!g || g->size() != 128) {
        intact = false;
        break;
      }
    }
    check(intact, "defrag: live records intact via tick pump");
    cap.clear();
    r.executeLine("debug.defrag bogus");
    check(cap.contains("unknown store"), "defrag: unknown store refused");
    r.setDefragService(nullptr);
    std::error_code ec;
    std::filesystem::remove(fragPath, ec);
  }

  // ---- completer: HL1-style tab completion (pure logic, no UI) ----
  {
    ConsoleRegistry r;
    std::string err;
    auto noop = [](ConsoleRegistry&, const ConsoleArgs&) {};
    bool okReg = r.registerCommand("alpha_one", "first alpha", noop, &err);
    okReg = r.registerCommand("alpha_two", "second alpha", noop, &err) && okReg;
    okReg = r.registerCommand("beta", "a beta command", noop, &err) && okReg;
    okReg = r.registerCVar("alpha_speed", CVarType::Int, "300", false, "alpha speed", &err) && okReg;
    okReg = r.setAlias("alpha_alias", "echo hi", &err) && okReg;
    // Script root for the `exec` context tests.
    const auto scriptDir = tempDir("completion");
    std::ofstream(scriptDir / "test_alpha.lth") << "echo a\n";
    std::ofstream(scriptDir / "test_beta.lth") << "echo b\n";
    std::ofstream(scriptDir / "other.cfg") << "echo c\n";
    std::ofstream(scriptDir / "notes.txt") << "not a script\n";
    r.setConfigDir(scriptDir);
    RegistryCompletionSource src(r);
    Completer comp(src);
    bool ok = okReg;

    // Common-prefix completion over commands + cvars + aliases.
    CompletionResult cr = comp.complete("al");
    ok = ok && cr.completedLine == "alpha_" && cr.matches.size() == 4 && !cr.singleExact;
    bool sawAlias = false, sawCvar = false;
    for (const auto& m : cr.matches) {
      if (m.name == "alpha_alias") sawAlias = sawAlias || m.kind == CompletionKind::Alias;
      if (m.name == "alpha_speed") sawCvar = sawCvar || m.kind == CompletionKind::Cvar;
    }
    ok = ok && sawAlias && sawCvar;
    // Rows come back sorted by name (kind breaks ties): deterministic order.
    ok = ok && cr.matches[0].name == "alpha_alias" && cr.matches[1].name == "alpha_one" &&
         cr.matches[2].name == "alpha_speed" && cr.matches[3].name == "alpha_two";
    check(ok, "completer: prefix match over commands+cvars+aliases, common-prefix completion, sorted rows");

    // Unique-but-inexact match: completes the full name (no trailing space yet).
    cr = comp.complete("alpha_o");
    ok = cr.matches.size() == 1 && cr.completedLine == "alpha_one" && !cr.singleExact;
    check(ok, "completer: unique inexact match completes the full name");

    // Single exact match: completed line unchanged, singleExact set (UI adds the space).
    cr = comp.complete("alpha_one");
    ok = cr.matches.size() == 1 && cr.completedLine == "alpha_one" && cr.singleExact;
    check(ok, "completer: single exact match flags singleExact for the trailing space");

    // Empty prefix: no-op. No matches: no-op.
    cr = comp.complete("");
    ok = cr.matches.empty() && cr.completedLine.empty();
    cr = comp.complete("zzz_no_such_thing");
    ok = ok && cr.matches.empty() && cr.completedLine == "zzz_no_such_thing";
    check(ok, "completer: empty prefix and no-match are no-ops");

    // Text after the first token is preserved.
    cr = comp.complete("alpha_o 1 2");
    ok = cr.completedLine == "alpha_one 1 2";
    check(ok, "completer: completion preserves arguments after the first token");

    // longestCommonPrefix unit.
    CompletionCandidate ca, cb;
    ca.name = "format";
    cb.name = "forward";
    ok = Completer::longestCommonPrefix({ca, cb}) == "for" &&
         Completer::longestCommonPrefix({}).empty();
    check(ok, "completer: longestCommonPrefix");

    // Cvar rows carry the CURRENT value as the detail; it tracks setCVar.
    auto rows = comp.filter("alpha_s");
    ok = rows.size() == 1 && rows[0].kind == CompletionKind::Cvar && rows[0].detail == "300";
    r.setCVar("alpha_speed", "400", &err);
    rows = comp.filter("alpha_s");
    ok = ok && rows.size() == 1 && rows[0].detail == "400";
    check(ok, "completer: cvar rows annotated with the live current value");

    // filter() dropdown rules: nothing to show on empty prefix, no matches,
    // or a single match that already equals the typed prefix.
    ok = comp.filter("").empty() && comp.filter("zzz_no_such_thing").empty() &&
         comp.filter("alpha_one").empty();
    rows = comp.filter("alpha_");
    ok = ok && rows.size() == 4;  // several matches -> rows shown
    check(ok, "completer: filter() hides empty-prefix / no-match / already-exact, shows the rest");

    // `exec` context: completes sandboxed script filenames only.
    cr = comp.complete("exec test_");
    ok = cr.matches.size() == 2 && cr.completedLine == "exec test_" &&
         cr.matches[0].kind == CompletionKind::File && cr.matches[0].name == "test_alpha.lth" &&
         cr.matches[1].name == "test_beta.lth";
    cr = comp.complete("exec test_a");
    ok = ok && cr.matches.size() == 1 && cr.completedLine == "exec test_alpha.lth";
    cr = comp.complete("exec notes");  // notes.txt is not a script: excluded
    ok = ok && cr.matches.empty();
    cr = comp.complete("exec other");
    ok = ok && cr.matches.size() == 1 && cr.matches[0].name == "other.cfg";  // legacy .cfg alias
    check(ok, "completer: after `exec` completes sandboxed .lth/.cfg filenames only");

    // `map` context: dimension names (built-in list, then an injected provider).
    cr = comp.complete("map wa");
    ok = cr.matches.size() == 1 && cr.matches[0].kind == CompletionKind::Dimension &&
         cr.matches[0].name == "wafer" && cr.completedLine == "map wafer";
    cr = comp.complete("map ");
    ok = ok && cr.matches.size() == 5;
    src.setDimensionProvider([] { return std::vector<std::string>{"custom_dim"}; });
    cr = comp.complete("map c");
    ok = ok && cr.matches.size() == 1 && cr.matches[0].name == "custom_dim";
    check(ok, "completer: after `map` completes dimension names (game-injectable)");

    // Line splitting + context-command predicate (shared with UI layers).
    const CompletionSplit sp = Completer::splitCompletionLine("exec  my file");
    ok = sp.head == "exec" && sp.rest == "my file" && sp.hasSpace;
    const CompletionSplit sp2 = Completer::splitCompletionLine("ec");
    ok = ok && sp2.head == "ec" && !sp2.hasSpace && sp2.rest.empty();
    ok = ok && Completer::isContextCommand("exec") && Completer::isContextCommand("map") &&
         !Completer::isContextCommand("echo");
    check(ok, "completer: splitCompletionLine + isContextCommand");
  }

  // ---- DebugConsole autocomplete dropdown (headless UiRoot) ----
  {
    ui::UiRoot ui;
    ConsoleRegistry r;
    DebugConsole con(r);
    con.attach(ui);  // registers builtins: echo, exec, ...
    con.open();
    bool ok = !con.completionVisible();

    // Typing filters the dropdown live ("unb" matches unbind + unbindall).
    con.setInputTextForTest("unb");
    ok = ok && con.completionVisible() && con.completionRowCount() == 2 &&
         con.completionSelected() == 0;
    // Nothing to show -> hidden.
    con.setInputTextForTest("zzz_no_such_thing");
    ok = ok && !con.completionVisible();
    con.setInputTextForTest("unbind");
    ok = ok && con.completionVisible();  // two matches (unbind, unbindall): still shown

    // Tab with an ambiguous prefix: completes the common prefix, matches listed.
    con.submitForTest("echo hi");  // seeds the log view (an empty buffer counts as one line)
    con.setInputTextForTest("unb");
    const size_t linesBefore = con.logLineCount();
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Tab)) && con.inputText() == "unbind" &&
         con.logLineCount() > linesBefore;
    // Tab with a unique match: completes plus the trailing space.
    con.setInputTextForTest("ech");
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Tab)) && con.inputText() == "echo ";
    // Tab with no matches: no-op.
    con.setInputTextForTest("zzz_no_such_thing");
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Tab)) &&
         con.inputText() == "zzz_no_such_thing";
    check(ok, "console dropdown: live filtering, tab ambiguous/unique/no-match");

    // Arrow keys move the dropdown selection; Enter accepts; Escape dismisses
    // the dropdown (console stays open); a second Escape closes the console.
    con.setInputTextForTest("unb");
    ok = con.completionVisible() && con.completionRowCount() == 2;
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Down)) && con.completionSelected() == 1;
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Up)) && con.completionSelected() == 0;
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Enter)) && con.inputText() == "unbind " &&
         !con.completionVisible();
    con.setInputTextForTest("ec");
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Escape)) && con.isOpen() &&
         !con.completionVisible();
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Escape)) && !con.isOpen();
    check(ok, "console dropdown: arrows move, enter accepts, escape dismisses then closes");

    // `exec` context in the live dropdown: script filenames.
    con.open();
    const auto scriptDir = tempDir("completion_ui");
    std::ofstream(scriptDir / "quest_start.lth") << "echo q\n";
    r.setConfigDir(scriptDir);
    con.setInputTextForTest("exec quest_");
    ok = con.completionVisible() && con.completionRowCount() == 1;
    ok = ok && con.handleKeyPreFilter(keyEvent(ui::Key::Enter)) &&
         con.inputText() == "exec quest_start.lth ";
    con.close();
    check(ok, "console dropdown: `exec` context completes script filenames in the UI");
  }

  // ---- delimiter pairing: block-comment auto-close + paste balancing ----
  // Pure editor logic for the Fragment script editor (JD spec 2026-10-11):
  // typing `/*` auto-inserts `*/` with the cursor inside; pasting lone
  // delimiters pairs them bidirectionally; strings, escapes, line
  // comments and open blocks are respected exactly like the tokenizer.
  {
    using SES = ScanEndState;
    bool ok = true;
    ok = scanEndState("") == SES::Normal && scanEndState("echo hi") == SES::Normal &&
         scanEndState("echo \"foo") == SES::InString &&
         scanEndState("echo \"foo\"") == SES::Normal && scanEndState("/* open") == SES::InBlock &&
         scanEndState("/* x */ done") == SES::Normal && scanEndState("// hi") == SES::InLineComment &&
         scanEndState("// hi\n") == SES::Normal;
    check(ok, "pairing: scanEndState reports Normal/InString/InBlock/InLineComment");

    // autoCloseOnType: real opener -> `*/` inserted, cursor inside.
    {
      const AutoCloseResult r = autoCloseOnType("echo /*", "");
      ok = r.applied && r.text == "echo /**/" && r.cursor == 7;
      const AutoCloseResult r2 = autoCloseOnType("/*", " world");
      ok = ok && r2.applied && r2.text == "/**/ world" && r2.cursor == 2;
      const AutoCloseResult r3 = autoCloseOnType("// x\n/*", "");
      ok = ok && r3.applied && r3.text == "// x\n/**/" && r3.cursor == 7;
      check(ok, "pairing: typing `/*` inserts `*/` and parks the cursor inside");
    }
    // autoCloseOnType: not a real opener -> no-op.
    {
      const AutoCloseResult s1 = autoCloseOnType("echo \"/*", "");
      const AutoCloseResult s2 = autoCloseOnType("/* /*", "");
      const AutoCloseResult s3 = autoCloseOnType("// /*", "");
      const AutoCloseResult s4 = autoCloseOnType("x = \\/*", "");
      const AutoCloseResult s5 = autoCloseOnType("say \"\\\"/*", "");
      ok = !s1.applied && s1.text == "echo \"/*" && !s2.applied && s2.text == "/* /*" &&
           !s3.applied && s3.text == "// /*" && !s4.applied && s4.text == "x = \\/*" &&
           !s5.applied && s5.text == "say \"\\\"/*";
      check(ok, "pairing: `/*` inside strings/escapes/line-comments/open blocks is inert");
    }
    // balancePastedDelimiters: lone delimiters pair up. Pairing is
    // adjacent (`/**/`), exactly like autoCloseOnType — no space is added.
    {
      ok = balancePastedDelimiters("/*", "") == "/**/";
      ok = ok && balancePastedDelimiters("*/", "") == "/**/";
      ok = ok && balancePastedDelimiters("/* hi */", "") == "/* hi */";
      ok = ok && balancePastedDelimiters("a /* b", "") == "a /* b*/";
      ok = ok && balancePastedDelimiters("*/ /*", "") == "/**/ /**/";
      ok = ok && balancePastedDelimiters("/* a /* b", "") == "/* a /* b*/";
      check(ok, "pairing: pasted lone `/*`/`*/` pair bidirectionally; balanced input untouched");
    }
    // balancePastedDelimiters: protection rules (no pairing from inert text).
    {
      ok = balancePastedDelimiters("\"/*\"", "") == "\"/*\"";
      ok = ok && balancePastedDelimiters("\\/*", "") == "\\/*";
      ok = ok && balancePastedDelimiters("// /*", "") == "// /*";
      ok = ok && balancePastedDelimiters("/*", "/* open") == "/*";
      ok = ok && balancePastedDelimiters("*/", "/* open") == "*/";
      ok = ok && balancePastedDelimiters("/*", "\"foo") == "/*";
      ok = ok && balancePastedDelimiters("/*", "// note") == "/*";
      check(ok, "pairing: strings/escapes/line-comments/open blocks never trigger pairing");
    }
    // balancePastedDelimiters: context transitions still pair correctly.
    {
      // Paste closes the string, then the `/*` is real -> paired.
      ok = balancePastedDelimiters("\" /*", "\"foo") == "\" /**/";
      // Paste lands in a line comment; the `*/` after the newline is lone.
      ok = ok && balancePastedDelimiters("/*\nfoo */", "// note") == "/*/*\nfoo */";
      check(ok, "pairing: paste context transitions (string/line-comment close) handled");
    }
    // trySkipOverCloser: VS Code-style skip-over, auto-inserted closers
    // ONLY (deliberate divergence from VS Code, which skips any matching
    // closer — a manually typed/pasted `*/` must never be hijacked).
    {
      bool ok = true;
      // (a) Full flow: auto-close, type `*` then `/` -> skip, no duplicate.
      {
        const AutoCloseResult ac = autoCloseOnType("echo /*", "");
        ok = ac.applied && ac.cursor == 7;
        // Editor records 7; typing `*` then `/` shifts the marker to 9.
        // At the call, `before` ends with the just-typed `*/`.
        const SkipOverResult s =
            trySkipOverCloser("echo /**/", "*/", std::unordered_set<size_t>{9});
        ok = ok && s.skipped && s.text == "echo /**/" && s.cursor == 9;
      }
      // (b) Typed `*/` but nothing to skip over -> no skip, literal insert.
      {
        const SkipOverResult s =
            trySkipOverCloser("/* a */", " b", std::unordered_set<size_t>{});
        ok = ok && !s.skipped && s.text == "/* a */ b" && s.cursor == 7;
      }
      // (f) No double-skip: after a skip the closer sits behind the cursor.
      {
        const SkipOverResult s =
            trySkipOverCloser("echo /**/", "", std::unordered_set<size_t>{7});
        ok = ok && !s.skipped && s.text == "echo /**/" && s.cursor == 9;
      }
      // (g) Text typed between the delimiters: the shifted marker skips.
      {
        const SkipOverResult s =
            trySkipOverCloser("/*hi*/", "*/", std::unordered_set<size_t>{6});
        ok = ok && s.skipped && s.text == "/*hi*/" && s.cursor == 6;
      }
      check(ok, "pairing: `*/` typed before an auto-inserted closer skips over it");
    }
    // trySkipOverCloser: safety — never hijack what the user meant.
    {
      bool ok = true;
      // (c) Inside a string the `*/` is literal text -> never skip,
      // even with a (stale) tracked position.
      {
        const SkipOverResult s = trySkipOverCloser(
            "say \"*/", "*/\"", std::unordered_set<size_t>{7});
        ok = !s.skipped && s.text == "say \"*/*/\"" && s.cursor == 7;
      }
      // (d) A manually typed `*/` ahead is never skipped: the typed
      // characters insert literally, exactly as requested.
      {
        const SkipOverResult s = trySkipOverCloser(
            "/* note */", "*/", std::unordered_set<size_t>{});
        ok = ok && !s.skipped && s.text == "/* note */*/" && s.cursor == 10;
      }
      // (e) Cursor not on a tracked closer position -> no skip.
      {
        const SkipOverResult s = trySkipOverCloser(
            "ab */", "*/", std::unordered_set<size_t>{99});
        ok = ok && !s.skipped && s.text == "ab */*/" && s.cursor == 5;
      }
      check(ok, "pairing: skip-over never hijacks manual closers or string text");
    }
    // Cross-check: the pairing scanner agrees with the tokenizer on a
    // battery of delimiter placements (same lexical rules, no drift).
    {
      const char* cases[] = {"/* a */", "\"/*\"", "\\/*", "// /*",  "/* /* */",
                             "x /* y",  "*/",     "a \"b /* c\" d", "/*\n*/"};
      ok = true;
      for (const char* c : cases) {
        std::string err;
        const auto comments = ConsoleRegistry::extractComments(c, &err);
        const SES st = scanEndState(c);
        // extractComments yields one body per real comment; the pairing
        // scanner must see the same comment structure: compare the end
        // state against the tokenizer's error/closure behavior.
        const bool tokenizerError = !err.empty();
        const bool scannerOpen = (st == SES::InBlock);
        // Both agree exactly when: error ⟺ scanner ends inside a block.
        // (`extractComments` keeps bodies extracted so far on error.)
        ok = ok && (tokenizerError == scannerOpen);
        (void)comments;
      }
      check(ok, "pairing: scanner agrees with extractComments on unterminated blocks");
    }
    // Fail conditions: degenerate inputs fail closed — safe defaults,
    // never a crash, never corrupted text.
    {
      bool ok = true;
      // Empty inputs are safe no-ops.
      {
        const AutoCloseResult e = autoCloseOnType("", "");
        ok = !e.applied && e.text.empty() && e.cursor == 0;
        const SkipOverResult s =
            trySkipOverCloser("", "", std::unordered_set<size_t>{});
        ok = ok && !s.skipped && s.text.empty() && s.cursor == 0;
        ok = ok && balancePastedDelimiters("", "").empty();
        ok = ok && balancePastedDelimiters("", "echo hi").empty();
        ok = ok && scanEndState("") == SES::Normal;
      }
      check(ok, "pairing: empty inputs fail closed (no-op, no crash)");
    }
    {
      // Absurd sizes (> 16 MiB cap) are refused without scanning.
      const std::string huge(17u * 1024u * 1024u, 'x');  // 17 MiB
      bool ok = true;
      {
        const AutoCloseResult h = autoCloseOnType(huge, "");
        ok = !h.applied && h.text.empty() && h.cursor == 0;
        const AutoCloseResult h2 = autoCloseOnType("/*", huge);
        ok = ok && !h2.applied && h2.text.empty() && h2.cursor == 0;
      }
      {
        const SkipOverResult s =
            trySkipOverCloser(huge, "", std::unordered_set<size_t>{});
        ok = ok && !s.skipped && s.text.empty() && s.cursor == 0;
      }
      // Paste balancing returns absurd input byte-identical, unpaired.
      ok = ok && balancePastedDelimiters(huge, "") == huge;
      ok = ok && balancePastedDelimiters("/*", huge) == "/*";
      ok = ok && scanEndState(huge) == SES::Normal;
      check(ok, "pairing: absurd input sizes fail closed (no scan, no crash)");
    }
  }

  return check.fails();
}

}  // namespace litholattice::console
