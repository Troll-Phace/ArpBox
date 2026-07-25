// ─────────────────────────────────────────────────────────────────────────────
// AllocationCounter.cpp — global operator new/delete REPLACEMENT, test-binary
// only (see AllocationSentinel.h for the harness contract and usage rules).
//
// Why this lives ONLY in the test executable:
//   Replacing the global replaceable allocation functions in the main executable
//   makes the whole process route heap traffic through here — including the
//   engine's `processBlock` path we want to prove allocation-free. A replacement
//   in the executable image wins over the libc++ default at link time on macOS,
//   so linking this TU into `arpbox_tests` is sufficient; it is NEVER compiled
//   into arpbox_engine / the app (that would ship a debugging hook in release).
//
// Composition with sanitizers: each override forwards to std::malloc / std::free,
// which BOTH ASan and TSan intercept — so heap-error and data-race detection on
// these allocations still work. What is lost under ASan is operator-new/delete
// mismatch detection specifically; that is acceptable. The zero-allocation guard
// test itself (infra_alloc_guard.cpp) is tagged [perf-budget] and is meant to run
// under the DEFAULT preset only — ASan does its own bookkeeping and the strict
// "== 0" assertion is only meaningful without a sanitizer replacing new.
//
// RT / thread-safety: all counters are THREAD-LOCAL, so these overrides introduce
// no shared mutable state and are themselves data-race free (important — they are
// live during the TSan stress tests, which spawn real threads).
// ─────────────────────────────────────────────────────────────────────────────

#include "AllocationSentinel.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace
{
// Thread-local so counting is scoped to the thread that armed it and needs no
// synchronisation. Zero-initialised, so any allocation before the harness is
// touched (static init, other threads) is simply not counted.
thread_local bool tlArmed = false;
thread_local std::uint64_t tlAllocations = 0;
thread_local std::uint64_t tlDeallocations = 0;

// Count-then-allocate. A zero-size request is bumped to 1 byte so every call
// returns a distinct, freeable pointer (standard-conforming).
inline void* countedAllocate (std::size_t size) noexcept
{
    if (tlArmed)
        ++tlAllocations;

    return std::malloc (size != 0 ? size : 1);
}

// Over-aligned count-then-allocate (C++17 std::align_val_t forms). posix_memalign
// returns memory that is freeable with std::free, so the aligned deletes below
// route through countedFree just like the plain ones. Alignment must be a power of
// two and at least sizeof(void*); over-aligned types always satisfy the former, and
// we bump up to the latter defensively.
inline void* countedAlignedAllocate (std::size_t size, std::align_val_t alignment) noexcept
{
    if (tlArmed)
        ++tlAllocations;

    std::size_t align = static_cast<std::size_t> (alignment);
    if (align < sizeof (void*))
        align = sizeof (void*);

    void* ptr = nullptr;
    if (posix_memalign (&ptr, align, size != 0 ? size : 1) != 0)
        return nullptr;

    return ptr;
}

// Count-then-free. Null is a no-op and is not counted. Frees both plain-malloc'd
// and posix_memalign'd pointers (both are std::free-compatible).
inline void countedFree (void* ptr) noexcept
{
    if (ptr == nullptr)
        return;

    if (tlArmed)
        ++tlDeallocations;

    std::free (ptr);
}
} // namespace

namespace arpbox::test
{
void setAllocationCounterArmed (bool armed) noexcept
{
    tlArmed = armed;
}
bool isAllocationCounterArmed () noexcept
{
    return tlArmed;
}
std::uint64_t allocationCount () noexcept
{
    return tlAllocations;
}
std::uint64_t deallocationCount () noexcept
{
    return tlDeallocations;
}
} // namespace arpbox::test

// ── Replaceable global allocation functions (C++ [new.delete]) ───────────────
// Throwing forms report failure with std::bad_alloc; nothrow forms return null.

void* operator new (std::size_t size)
{
    if (void* const p = countedAllocate (size))
        return p;
    throw std::bad_alloc ();
}

void* operator new[] (std::size_t size)
{
    if (void* const p = countedAllocate (size))
        return p;
    throw std::bad_alloc ();
}

void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    return countedAllocate (size);
}

void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    return countedAllocate (size);
}

void operator delete (void* ptr) noexcept
{
    countedFree (ptr);
}
void operator delete[] (void* ptr) noexcept
{
    countedFree (ptr);
}

// Sized-deallocation overloads (C++14): define them so the compiler cannot pick a
// default that bypasses our free counting.
void operator delete (void* ptr, std::size_t) noexcept
{
    countedFree (ptr);
}
void operator delete[] (void* ptr, std::size_t) noexcept
{
    countedFree (ptr);
}

void operator delete (void* ptr, const std::nothrow_t&) noexcept
{
    countedFree (ptr);
}
void operator delete[] (void* ptr, const std::nothrow_t&) noexcept
{
    countedFree (ptr);
}

// Over-aligned overloads (C++17 [new.delete] with std::align_val_t). Without these
// an over-aligned allocation on a measured path would bypass the counter — a
// false-negative for the zero-allocation guard.

void* operator new (std::size_t size, std::align_val_t align)
{
    if (void* const p = countedAlignedAllocate (size, align))
        return p;
    throw std::bad_alloc ();
}

void* operator new[] (std::size_t size, std::align_val_t align)
{
    if (void* const p = countedAlignedAllocate (size, align))
        return p;
    throw std::bad_alloc ();
}

void* operator new (std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept
{
    return countedAlignedAllocate (size, align);
}

void* operator new[] (std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept
{
    return countedAlignedAllocate (size, align);
}

void operator delete (void* ptr, std::align_val_t) noexcept
{
    countedFree (ptr);
}
void operator delete[] (void* ptr, std::align_val_t) noexcept
{
    countedFree (ptr);
}

// Sized + aligned deallocation forms.
void operator delete (void* ptr, std::size_t, std::align_val_t) noexcept
{
    countedFree (ptr);
}
void operator delete[] (void* ptr, std::size_t, std::align_val_t) noexcept
{
    countedFree (ptr);
}

// Nothrow + aligned deallocation forms (pair with the nothrow aligned news).
void operator delete (void* ptr, std::align_val_t, const std::nothrow_t&) noexcept
{
    countedFree (ptr);
}
void operator delete[] (void* ptr, std::align_val_t, const std::nothrow_t&) noexcept
{
    countedFree (ptr);
}
