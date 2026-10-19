// Replaceable global operator new/delete: per-mod memory-quota attribution for
// every C++ heap allocation.
//
// Background: the mod-hardening program attributes AngelScript allocations to
// the executing mod via asSetGlobalMemoryFunctions shims (script_engine.cpp).
// That left a blind spot: the script string add-on's std::string heap buffers
// (and any other standard-container memory) go through the global operator
// new, bypassing the AngelScript memory functions, so unbounded strings
// escaped the quota. This file closes it by extending the same thread-local
// attribution (tlsScriptAllocOwner, set around ScriptEngine::run()) to the
// global operators.
//
// Design:
// - Every allocation carries a small header placed immediately before the
//   returned payload: the malloc'd block to free, the requested size, and the
//   owning mod (null outside mod execution). The payload honors the requested
//   alignment, including align_val_t overloads.
// - When no mod is attributed on this thread (the common case) the header
//   records a null owner and no accounting runs: engine allocations behave
//   exactly like the default operators, byte for byte.
// - No recursion: the header scheme is malloc/free only, and
//   quotaCharge/quotaCredit are pure integer arithmetic, so accounting never
//   allocates through the hooked path. quotaEnforce() may log (which
//   allocates); it runs under the tlsQuotaHookDepth guard, and allocations
//   made while the guard is held are still charged, just not re-checked, so
//   the books always balance.
// - No double counting: AngelScript's own memory shims use malloc/free
//   directly and never pass through these operators, so each allocation is
//   attributed at exactly one point. Sized and unsized delete both read the
//   header (the authoritative size), so neither double-counts nor leaks.
//
// Honest limits (also in script_engine.h):
// - SSO strings never heap-allocate: nothing to count, correctly zero.
// - Allocations on threads with no attributed mod are unattributed by design.
// - Third-party libraries with custom allocators (mmap/pools) bypass this.
// - This replaces the process-wide operators for every binary that links
//   litholattice_script (the game and the tests); MSVC builds get the same
//   replacement via the standard replaceable-function mechanism.

#include "script_memory_internal.h"

#include <cstdint>
#include <memory>
#include <cstdlib>
#include <new>

#include "litholattice/script/script_engine.h"

namespace litholattice {

// One definition; declared extern in script_memory_internal.h.
thread_local ScriptEngine* tlsScriptAllocOwner = nullptr;

thread_local unsigned tlsQuotaHookDepth = 0;

namespace detail {

// 24 bytes on 64-bit (3 pointers/size_ts). Sits immediately before the payload.
struct QuotaHeader {
  void* raw;                                  // malloc'd block, passed to free() on delete
  std::size_t size;                           // payload bytes the caller requested
  std::shared_ptr<QuotaAccount> account;      // attributed mod's account, or null outside mod execution
};

constexpr std::size_t kHeaderSize = sizeof(QuotaHeader);

void* quotaAllocate(std::size_t n, std::size_t alignment, bool nothrow) {
  if (alignment < alignof(void*) || (alignment & (alignment - 1)) != 0)
    alignment = alignof(std::max_align_t);  // defensive: align_val_t is always a power of two
  ScriptEngine* owner = tlsScriptAllocOwner;
  const std::size_t pad = kHeaderSize + alignment;
  if (n > static_cast<std::size_t>(-1) - pad) {  // padded total would overflow
    if (nothrow) return nullptr;
    throw std::bad_alloc();
  }
  void* raw = nullptr;
  while ((raw = std::malloc(pad + n)) == nullptr) {
    // Same contract as the default operator new: consult the new-handler.
    std::new_handler handler = std::get_new_handler();
    if (!handler) {
      if (nothrow) return nullptr;
      throw std::bad_alloc();
    }
    handler();  // may throw, or return and let us retry
  }
  // Payload starts after the header, rounded up to the requested alignment.
  // pad + n did not overflow, so base + alignment - 1 cannot either.
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(raw) + kHeaderSize;
  const std::uintptr_t payloadAddr = (base + alignment - 1) & ~(alignment - 1);
  auto* header = reinterpret_cast<QuotaHeader*>(payloadAddr - kHeaderSize);
  header->raw = raw;
  header->size = n;
  // The header sits in raw malloc'd memory: the shared_ptr must be
  // placement-new constructed (assigning to unconstructed storage is UB).
  if (owner != nullptr) {
    new (&header->account) std::shared_ptr<QuotaAccount>(owner->quotaAccount());
    owner->quotaCharge(n);
    if (tlsQuotaHookDepth == 0) {
      // Enforcement may log, and logging allocates: hold the reentrancy
      // guard so nested allocations charge but do not re-enforce.
      QuotaHookGuard guard;
      owner->quotaEnforce();
    }
  } else {
    new (&header->account) std::shared_ptr<QuotaAccount>();
  }
  return reinterpret_cast<void*>(payloadAddr);
}

void quotaDeallocate(void* p) noexcept {
  if (p == nullptr) return;
  auto* header = reinterpret_cast<QuotaHeader*>(static_cast<char*>(p) - kHeaderSize);
  const std::size_t n = header->size;
  void* raw = header->raw;
  // Credit via the shared account, NOT the engine: the engine may be destroyed
  // already (mod unload with live allocations). The shared_ptr keeps the
  // account alive; this is pure arithmetic and cannot allocate or throw.
  // The header's shared_ptr was placement-new constructed: move it out, credit,
  // then explicitly destroy it (raw memory has no automatic destructor call).
  std::shared_ptr<QuotaAccount> account = std::move(header->account);
  header->account.~shared_ptr<QuotaAccount>();
  if (account) {
    account->bytes -= static_cast<int64_t>(n);
    if (account->bytes < 0) account->bytes = 0;
  }
  std::free(raw);
}

}  // namespace detail
}  // namespace litholattice

// ---- Replaceable allocation functions (C++ [replacement.functions]) ---------
// Global scope: these replace the default operators process-wide in any binary
// linking litholattice_script. All flavors route to the same header scheme so
// new/delete, new[]/delete[], sized/unsized, aligned, and nothrow forms pair
// correctly and account exactly once.

void* operator new(std::size_t n) {
  return litholattice::detail::quotaAllocate(n, alignof(std::max_align_t), false);
}
void* operator new[](std::size_t n) {
  return litholattice::detail::quotaAllocate(n, alignof(std::max_align_t), false);
}
void* operator new(std::size_t n, std::align_val_t a) {
  return litholattice::detail::quotaAllocate(n, static_cast<std::size_t>(a), false);
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return litholattice::detail::quotaAllocate(n, static_cast<std::size_t>(a), false);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  return litholattice::detail::quotaAllocate(n, alignof(std::max_align_t), true);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  return litholattice::detail::quotaAllocate(n, alignof(std::max_align_t), true);
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return litholattice::detail::quotaAllocate(n, static_cast<std::size_t>(a), true);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return litholattice::detail::quotaAllocate(n, static_cast<std::size_t>(a), true);
}

void operator delete(void* p) noexcept { litholattice::detail::quotaDeallocate(p); }
void operator delete[](void* p) noexcept { litholattice::detail::quotaDeallocate(p); }
// Sized delete: the header size is authoritative (it is what the matching new
// recorded), so the passed size is only a consistency check opportunity.
void operator delete(void* p, std::size_t) noexcept { litholattice::detail::quotaDeallocate(p); }
void operator delete[](void* p, std::size_t) noexcept { litholattice::detail::quotaDeallocate(p); }
void operator delete(void* p, std::align_val_t) noexcept { litholattice::detail::quotaDeallocate(p); }
void operator delete[](void* p, std::align_val_t) noexcept { litholattice::detail::quotaDeallocate(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  litholattice::detail::quotaDeallocate(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  litholattice::detail::quotaDeallocate(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept { litholattice::detail::quotaDeallocate(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { litholattice::detail::quotaDeallocate(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  litholattice::detail::quotaDeallocate(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  litholattice::detail::quotaDeallocate(p);
}
