/*
 * asb_atomics.h — force the _Interlocked* atomics to INLINE intrinsics (MSVC/Windows only).
 *
 * WHY: with the static CRT (/MT, libcmt) the ARM64 compiler emits "outlined atomics" — CALLS into a
 * libcmt helper that does runtime LSE dispatch. That helper faults with STATUS_ILLEGAL_INSTRUCTION
 * (0xc000001d) in our guest binaries. Forcing the intrinsics inline (casal / ldxr-stxr emitted
 * directly) bypasses the broken helper and works regardless of CRT linkage.
 *
 * Pulled in via asb_transport.h, so EVERY transport-linking Windows binary (agent, the channel EXEs,
 * p9copy, the VDD) is covered from one place — including each binary's OWN Interlocked* uses, because
 * #pragma intrinsic is in effect for the whole translation unit once this header is included near the
 * top. No-op off MSVC.
 */
#ifndef ASB_ATOMICS_H
#define ASB_ATOMICS_H

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_InterlockedCompareExchange, _InterlockedExchange, _InterlockedExchangeAdd)
#pragma intrinsic(_InterlockedIncrement, _InterlockedDecrement)
#pragma intrinsic(_InterlockedOr, _InterlockedAnd, _InterlockedXor)
#pragma intrinsic(_InterlockedCompareExchange64, _InterlockedExchange64)
#pragma intrinsic(_InterlockedExchangePointer, _InterlockedCompareExchangePointer)
#endif /* _MSC_VER */

#endif /* ASB_ATOMICS_H */
