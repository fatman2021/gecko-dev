/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Allocator.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/OperatorNewExtensions.h"
#include "mozilla/TimeStamp.h"

#include "gc/GCInternals.h"
#include "gc/GCLock.h"
#include "gc/GCProbes.h"
#include "gc/Nursery.h"
#include "threading/CpuCount.h"
#include "util/Poison.h"
#include "vm/BigIntType.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"
#include "vm/StringType.h"

#include "gc/ArenaList-inl.h"
#include "gc/Heap-inl.h"
#include "gc/PrivateIterators-inl.h"
#include "vm/JSContext-inl.h"

using mozilla::TimeDuration;
using mozilla::TimeStamp;

using namespace js;
using namespace gc;

template <AllowGC allowGC /* = CanGC */>
JSObject* gc::CellAllocator::AllocateObject(JSContext* cx, AllocKind kind,
                                            size_t nDynamicSlots,
                                            gc::InitialHeap heap,
                                            const JSClass* clasp,
                                            AllocSite* site /* = nullptr */) {
  MOZ_ASSERT(!cx->isHelperThreadContext());
  MOZ_ASSERT(IsObjectAllocKind(kind));
  size_t thingSize = Arena::thingSize(kind);

  MOZ_ASSERT(thingSize == Arena::thingSize(kind));
  MOZ_ASSERT(thingSize >= sizeof(JSObject_Slots0));
  static_assert(
      sizeof(JSObject_Slots0) >= MinCellSize,
      "All allocations must be at least the allocator-imposed minimum size.");

  MOZ_ASSERT_IF(nDynamicSlots != 0, clasp->isNativeObject());

  MOZ_ASSERT_IF(site && site->initialHeap() == TenuredHeap,
                heap == TenuredHeap);

  JSRuntime* rt = cx->runtime();
  if (!rt->gc.checkAllocatorState<allowGC>(cx, kind)) {
    return nullptr;
  }

  if (cx->nursery().isEnabled() && heap != TenuredHeap) {
    if (!site) {
      site = cx->zone()->unknownAllocSite();
    }

    JSObject* obj = rt->gc.tryNewNurseryObject<allowGC>(
        cx, thingSize, nDynamicSlots, clasp, site);
    if (obj) {
      return obj;
    }

    // Our most common non-jit allocation path is NoGC; thus, if we fail the
    // alloc and cannot GC, we *must* return nullptr here so that the caller
    // will do a CanGC allocation to clear the nursery. Failing to do so will
    // cause all allocations on this path to land in Tenured, and we will not
    // get the benefit of the nursery.
    if (!allowGC) {
      return nullptr;
    }
  }

  return GCRuntime::tryNewTenuredObject<allowGC>(cx, kind, thingSize,
                                                 nDynamicSlots);
}
template JSObject* gc::CellAllocator::AllocateObject<NoGC>(
    JSContext* cx, gc::AllocKind kind, size_t nDynamicSlots,
    gc::InitialHeap heap, const JSClass* clasp, gc::AllocSite* site);
template JSObject* gc::CellAllocator::AllocateObject<CanGC>(
    JSContext* cx, gc::AllocKind kind, size_t nDynamicSlots,
    gc::InitialHeap heap, const JSClass* clasp, gc::AllocSite* site);

// Attempt to allocate a new JSObject out of the nursery. If there is not
// enough room in the nursery or there is an OOM, this method will return
// nullptr.
template <AllowGC allowGC>
JSObject* GCRuntime::tryNewNurseryObject(JSContext* cx, size_t thingSize,
                                         size_t nDynamicSlots,
                                         const JSClass* clasp,
                                         AllocSite* site) {
  MOZ_ASSERT(cx->isNurseryAllocAllowed());
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  JSObject* obj =
      cx->nursery().allocateObject(site, thingSize, nDynamicSlots, clasp);
  if (obj) {
    return obj;
  }

  if (allowGC && !cx->suppressGC) {
    cx->runtime()->gc.minorGC(JS::GCReason::OUT_OF_NURSERY);

    // Exceeding gcMaxBytes while tenuring can disable the Nursery.
    if (cx->nursery().isEnabled()) {
      return cx->nursery().allocateObject(site, thingSize, nDynamicSlots,
                                          clasp);
    }
  }
  return nullptr;
}

template <AllowGC allowGC>
JSObject* GCRuntime::tryNewTenuredObject(JSContext* cx, AllocKind kind,
                                         size_t thingSize,
                                         size_t nDynamicSlots) {
  ObjectSlots* slotsHeader = nullptr;
  if (nDynamicSlots) {
    HeapSlot* allocation =
        cx->maybe_pod_malloc<HeapSlot>(ObjectSlots::allocCount(nDynamicSlots));
    if (MOZ_UNLIKELY(!allocation)) {
      if (allowGC) {
        ReportOutOfMemory(cx);
      }
      return nullptr;
    }

    slotsHeader = new (allocation) ObjectSlots(nDynamicSlots, 0);
    Debug_SetSlotRangeToCrashOnTouch(slotsHeader->slots(), nDynamicSlots);
  }

  TenuredCell* cell = tryNewTenuredThing<allowGC>(cx, kind, thingSize);
  if (!cell) {
    js_free(slotsHeader);
    return nullptr;
  }

  if (nDynamicSlots) {
    NativeObject* nobj = new (mozilla::KnownNotNull, cell) NativeObject();
    nobj->initSlots(slotsHeader->slots());
    AddCellMemory(nobj, ObjectSlots::allocSize(nDynamicSlots),
                  MemoryUse::ObjectSlots);
    return nobj;
  }

  return new (mozilla::KnownNotNull, cell) JSObject();
}

// Attempt to allocate a new string out of the nursery. If there is not enough
// room in the nursery or there is an OOM, this method will return nullptr.
template <AllowGC allowGC>
Cell* GCRuntime::tryNewNurseryStringCell(JSContext* cx, size_t thingSize,
                                         AllocKind kind) {
  MOZ_ASSERT(IsNurseryAllocable(kind));
  MOZ_ASSERT(cx->isNurseryAllocAllowed());
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  AllocSite* site = cx->zone()->unknownAllocSite();
  Cell* cell = cx->nursery().allocateString(site, thingSize);
  if (cell) {
    return cell;
  }

  if (allowGC && !cx->suppressGC) {
    cx->runtime()->gc.minorGC(JS::GCReason::OUT_OF_NURSERY);

    // Exceeding gcMaxBytes while tenuring can disable the Nursery, and
    // other heuristics can disable nursery strings for this zone.
    if (cx->nursery().isEnabled() && cx->zone()->allocNurseryStrings) {
      return cx->nursery().allocateString(site, thingSize);
    }
  }
  return nullptr;
}

template <AllowGC allowGC /* = CanGC */>
Cell* gc::CellAllocator::AllocateStringCell(JSContext* cx, AllocKind kind,
                                            size_t size, InitialHeap heap) {
  MOZ_ASSERT(!cx->isHelperThreadContext());
  MOZ_ASSERT(size == Arena::thingSize(kind));
  MOZ_ASSERT(size == sizeof(JSString) || size == sizeof(JSFatInlineString));
  MOZ_ASSERT(
      IsNurseryAllocable(kind));  // Atoms are allocated using Allocate().

  JSRuntime* rt = cx->runtime();
  if (!rt->gc.checkAllocatorState<allowGC>(cx, kind)) {
    return nullptr;
  }

  if (cx->nursery().isEnabled() && heap != TenuredHeap &&
      cx->nursery().canAllocateStrings() && cx->zone()->allocNurseryStrings) {
    Cell* cell = rt->gc.tryNewNurseryStringCell<allowGC>(cx, size, kind);
    if (cell) {
      return cell;
    }

    // Our most common non-jit allocation path is NoGC; thus, if we fail the
    // alloc and cannot GC, we *must* return nullptr here so that the caller
    // will do a CanGC allocation to clear the nursery. Failing to do so will
    // cause all allocations on this path to land in Tenured, and we will not
    // get the benefit of the nursery.
    if (!allowGC) {
      return nullptr;
    }
  }

  return GCRuntime::tryNewTenuredThing<allowGC>(cx, kind, size);
}

template Cell* gc::CellAllocator::AllocateStringCell<NoGC>(JSContext*,
                                                           AllocKind, size_t,
                                                           InitialHeap);
template Cell* gc::CellAllocator::AllocateStringCell<CanGC>(JSContext*,
                                                            AllocKind, size_t,
                                                            InitialHeap);

// Attempt to allocate a new BigInt out of the nursery. If there is not enough
// room in the nursery or there is an OOM, this method will return nullptr.
template <AllowGC allowGC>
Cell* GCRuntime::tryNewNurseryBigIntCell(JSContext* cx, size_t thingSize,
                                         AllocKind kind) {
  MOZ_ASSERT(IsNurseryAllocable(kind));
  MOZ_ASSERT(cx->isNurseryAllocAllowed());
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  AllocSite* site = cx->zone()->unknownAllocSite();
  Cell* cell = cx->nursery().allocateBigInt(site, thingSize);
  if (cell) {
    return cell;
  }

  if (allowGC && !cx->suppressGC) {
    cx->runtime()->gc.minorGC(JS::GCReason::OUT_OF_NURSERY);

    // Exceeding gcMaxBytes while tenuring can disable the Nursery, and
    // other heuristics can disable nursery BigInts for this zone.
    if (cx->nursery().isEnabled() && cx->zone()->allocNurseryBigInts) {
      Cell* cell = cx->nursery().allocateBigInt(site, thingSize);
      if (cell) {
        return cell;
      }
    }
  }
  return nullptr;
}

template <AllowGC allowGC /* = CanGC */>
static Cell* AllocateBigIntCell(JSContext* cx, InitialHeap heap) {
  MOZ_ASSERT(!cx->isHelperThreadContext());

  AllocKind kind = MapTypeToAllocKind<JS::BigInt>::kind;
  size_t size = sizeof(JS::BigInt);
  MOZ_ASSERT(size == Arena::thingSize(kind));

  JSRuntime* rt = cx->runtime();
  if (!rt->gc.checkAllocatorState<allowGC>(cx, kind)) {
    return nullptr;
  }

  if (cx->nursery().isEnabled() && heap != TenuredHeap &&
      cx->nursery().canAllocateBigInts() && cx->zone()->allocNurseryBigInts) {
    Cell* cell = rt->gc.tryNewNurseryBigIntCell<allowGC>(cx, size, kind);
    if (cell) {
      return cell;
    }

    // Our most common non-jit allocation path is NoGC; thus, if we fail the
    // alloc and cannot GC, we *must* return nullptr here so that the caller
    // will do a CanGC allocation to clear the nursery. Failing to do so will
    // cause all allocations on this path to land in Tenured, and we will not
    // get the benefit of the nursery.
    if constexpr (!allowGC) {
      return nullptr;
    }
  }

  return GCRuntime::tryNewTenuredThing<allowGC>(cx, kind, size);
}

template <AllowGC allowGC /* = CanGC */>
JS::BigInt* gc::CellAllocator::AllocateBigInt(JSContext* cx, InitialHeap heap) {
  Cell* cell = AllocateBigIntCell<allowGC>(cx, heap);
  if (cell) {
    return new (mozilla::KnownNotNull, cell) JS::BigInt();
  }
  return nullptr;
}
template JS::BigInt* gc::CellAllocator::AllocateBigInt<NoGC>(
    JSContext* cx, gc::InitialHeap heap);
template JS::BigInt* gc::CellAllocator::AllocateBigInt<CanGC>(
    JSContext* cx, gc::InitialHeap heap);

template <AllowGC allowGC /* = CanGC */>
TenuredCell* gc::detail::AllocateTenuredCell(JSContext* cx, gc::AllocKind kind,
                                             size_t size) {
  MOZ_ASSERT(!cx->isHelperThreadContext());
  MOZ_ASSERT(!IsNurseryAllocable(kind));
  MOZ_ASSERT(size == Arena::thingSize(kind));
  MOZ_ASSERT(
      size >= gc::MinCellSize,
      "All allocations must be at least the allocator-imposed minimum size.");

  if (!cx->runtime()->gc.checkAllocatorState<allowGC>(cx, kind)) {
    return nullptr;
  }

  return GCRuntime::tryNewTenuredThing<allowGC>(cx, kind, size);
}

template TenuredCell* gc::detail::AllocateTenuredCell<NoGC>(JSContext*,
                                                            AllocKind, size_t);
template TenuredCell* gc::detail::AllocateTenuredCell<CanGC>(JSContext*,
                                                             AllocKind, size_t);

template <AllowGC allowGC>
/* static */
TenuredCell* GCRuntime::tryNewTenuredThing(JSContext* cx, AllocKind kind,
                                           size_t thingSize) {
  // Bump allocate in the arena's current free-list span.
  Zone* zone = cx->zone();
  void* t = zone->arenas.freeLists().allocate(kind);
  if (MOZ_UNLIKELY(!t)) {
    // Get the next available free list and allocate out of it. This may
    // acquire a new arena, which will lock the chunk list. If there are no
    // chunks available it may also allocate new memory directly.
    t = refillFreeList(cx, kind);

    if (MOZ_UNLIKELY(!t)) {
      if constexpr (allowGC) {
        cx->runtime()->gc.attemptLastDitchGC(cx);
        TenuredCell* cell = tryNewTenuredThing<NoGC>(cx, kind, thingSize);
        if (cell) {
          return cell;
        }
        ReportOutOfMemory(cx);
      }

      return nullptr;
    }
  }

  TenuredCell* cell = new (mozilla::KnownNotNull, t) TenuredCell();
  checkIncrementalZoneState(cx, cell);
  gcprobes::TenuredAlloc(cell, kind);
  // We count this regardless of the profiler's state, assuming that it costs
  // just as much to count it, as to check the profiler's state and decide not
  // to count it.
  zone->noteTenuredAlloc();
  return cell;
}

void GCRuntime::attemptLastDitchGC(JSContext* cx) {
  // Either there was no memory available for a new chunk or the heap hit its
  // size limit. Try to perform an all-compartments, non-incremental, shrinking
  // GC and wait for it to finish.

  if (!lastLastDitchTime.IsNull() &&
      TimeStamp::Now() - lastLastDitchTime <= tunables.minLastDitchGCPeriod()) {
    return;
  }

  JS::PrepareForFullGC(cx);
  gc(JS::GCOptions::Shrink, JS::GCReason::LAST_DITCH);
  waitBackgroundAllocEnd();
  waitBackgroundFreeEnd();

  lastLastDitchTime = mozilla::TimeStamp::Now();
}

template <AllowGC allowGC>
bool GCRuntime::checkAllocatorState(JSContext* cx, AllocKind kind) {
  if (allowGC) {
    if (!gcIfNeededAtAllocation(cx)) {
      return false;
    }
  }

#if defined(JS_GC_ZEAL) || defined(DEBUG)
  MOZ_ASSERT_IF(cx->zone()->isAtomsZone(),
                kind == AllocKind::ATOM || kind == AllocKind::FAT_INLINE_ATOM ||
                    kind == AllocKind::SYMBOL || kind == AllocKind::JITCODE ||
                    kind == AllocKind::SCOPE);
  MOZ_ASSERT_IF(!cx->zone()->isAtomsZone(),
                kind != AllocKind::ATOM && kind != AllocKind::FAT_INLINE_ATOM);
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy());
#endif

  // Crash if we perform a GC action when it is not safe.
  if (allowGC && !cx->suppressGC) {
    cx->verifyIsSafeToGC();
  }

  // For testing out of memory conditions
  if (js::oom::ShouldFailWithOOM()) {
    // If we are doing a fallible allocation, percolate up the OOM
    // instead of reporting it.
    if (allowGC) {
      ReportOutOfMemory(cx);
    }
    return false;
  }

  return true;
}

inline bool GCRuntime::gcIfNeededAtAllocation(JSContext* cx) {
#ifdef JS_GC_ZEAL
  if (needZealousGC()) {
    runDebugGC();
  }
#endif

  // Invoking the interrupt callback can fail and we can't usefully
  // handle that here. Just check in case we need to collect instead.
  if (cx->hasAnyPendingInterrupt()) {
    gcIfRequested();
  }

  return true;
}

template <typename T>
/* static */
void GCRuntime::checkIncrementalZoneState(JSContext* cx, T* t) {
#ifdef DEBUG
  MOZ_ASSERT(t);
  TenuredCell* cell = &t->asTenured();
  Zone* zone = cell->zone();
  if (zone->isGCMarkingOrSweeping()) {
    MOZ_ASSERT(cell->isMarkedBlack());
  } else {
    MOZ_ASSERT(!cell->isMarkedAny());
  }
#endif
}

TenuredCell* js::gc::AllocateCellInGC(Zone* zone, AllocKind thingKind) {
  TenuredCell* cell = zone->arenas.allocateFromFreeList(thingKind);
  if (!cell) {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    cell = GCRuntime::refillFreeListInGC(zone, thingKind);
    if (!cell) {
      oomUnsafe.crash(ChunkSize, "Failed to allocate new chunk during GC");
    }
  }
  return cell;
}

// ///////////  Arena -> Thing Allocator  //////////////////////////////////////

void GCRuntime::startBackgroundAllocTaskIfIdle() {
  AutoLockHelperThreadState lock;
  if (!allocTask.wasStarted(lock)) {
    // Join the previous invocation of the task. This will return immediately
    // if the thread has never been started.
    allocTask.joinWithLockHeld(lock);
    allocTask.startWithLockHeld(lock);
  }
}

/* static */
TenuredCell* GCRuntime::refillFreeList(JSContext* cx, AllocKind thingKind) {
  MOZ_ASSERT(cx->zone()->arenas.freeLists().isEmpty(thingKind));
  MOZ_ASSERT(!cx->isHelperThreadContext());

  // It should not be possible to allocate on the main thread while we are
  // inside a GC.
  MOZ_ASSERT(!JS::RuntimeHeapIsBusy(), "allocating while under GC");

  return cx->zone()->arenas.refillFreeListAndAllocate(
      thingKind, ShouldCheckThresholds::CheckThresholds);
}

/* static */
TenuredCell* GCRuntime::refillFreeListInGC(Zone* zone, AllocKind thingKind) {
  // Called by compacting GC to refill a free list while we are in a GC.
  MOZ_ASSERT(JS::RuntimeHeapIsCollecting());
  MOZ_ASSERT_IF(!JS::RuntimeHeapIsMinorCollecting(),
                !zone->runtimeFromMainThread()->gc.isBackgroundSweeping());

  return zone->arenas.refillFreeListAndAllocate(
      thingKind, ShouldCheckThresholds::DontCheckThresholds);
}

TenuredCell* ArenaLists::refillFreeListAndAllocate(
    AllocKind thingKind, ShouldCheckThresholds checkThresholds) {
  MOZ_ASSERT(freeLists().isEmpty(thingKind));

  JSRuntime* rt = runtimeFromAnyThread();

  mozilla::Maybe<AutoLockGCBgAlloc> maybeLock;

  // See if we can proceed without taking the GC lock.
  if (concurrentUse(thingKind) != ConcurrentUse::None) {
    maybeLock.emplace(rt);
  }

  Arena* arena = arenaList(thingKind).takeNextArena();
  if (arena) {
    // Empty arenas should be immediately freed.
    MOZ_ASSERT(!arena->isEmpty());

    return freeLists().setArenaAndAllocate(arena, thingKind);
  }

  // Parallel threads have their own ArenaLists, but chunks are shared;
  // if we haven't already, take the GC lock now to avoid racing.
  if (maybeLock.isNothing()) {
    maybeLock.emplace(rt);
  }

  TenuredChunk* chunk = rt->gc.pickChunk(maybeLock.ref());
  if (!chunk) {
    return nullptr;
  }

  // Although our chunk should definitely have enough space for another arena,
  // there are other valid reasons why TenuredChunk::allocateArena() may fail.
  arena = rt->gc.allocateArena(chunk, zone_, thingKind, checkThresholds,
                               maybeLock.ref());
  if (!arena) {
    return nullptr;
  }

  ArenaList& al = arenaList(thingKind);
  MOZ_ASSERT(al.isCursorAtEnd());
  al.insertBeforeCursor(arena);

  return freeLists().setArenaAndAllocate(arena, thingKind);
}

inline TenuredCell* FreeLists::setArenaAndAllocate(Arena* arena,
                                                   AllocKind kind) {
#ifdef DEBUG
  auto old = freeLists_[kind];
  if (!old->isEmpty()) {
    old->getArena()->checkNoMarkedFreeCells();
  }
#endif

  FreeSpan* span = arena->getFirstFreeSpan();
  freeLists_[kind] = span;

  Zone* zone = arena->zone;
  if (MOZ_UNLIKELY(zone->isGCMarkingOrSweeping())) {
    arena->arenaAllocatedDuringGC();
  }

  TenuredCell* thing = span->allocate(Arena::thingSize(kind));
  MOZ_ASSERT(thing);  // This allocation is infallible.

  return thing;
}

void Arena::arenaAllocatedDuringGC() {
  // Ensure that anything allocated during the mark or sweep phases of an
  // incremental GC will be marked black by pre-marking all free cells in the
  // arena we are about to allocate from.

  MOZ_ASSERT(zone->isGCMarkingOrSweeping());
  for (ArenaFreeCellIter cell(this); !cell.done(); cell.next()) {
    MOZ_ASSERT(!cell->isMarkedAny());
    cell->markBlack();
  }
}

// ///////////  TenuredChunk -> Arena Allocator  ///////////////////////////////

bool GCRuntime::wantBackgroundAllocation(const AutoLockGC& lock) const {
  // To minimize memory waste, we do not want to run the background chunk
  // allocation if we already have some empty chunks or when the runtime has
  // a small heap size (and therefore likely has a small growth rate).
  return allocTask.enabled() &&
         emptyChunks(lock).count() < minEmptyChunkCount(lock) &&
         (fullChunks(lock).count() + availableChunks(lock).count()) >= 4;
}

Arena* GCRuntime::allocateArena(TenuredChunk* chunk, Zone* zone,
                                AllocKind thingKind,
                                ShouldCheckThresholds checkThresholds,
                                const AutoLockGC& lock) {
  MOZ_ASSERT(chunk->hasAvailableArenas());

  // Fail the allocation if we are over our heap size limits.
  if ((checkThresholds != ShouldCheckThresholds::DontCheckThresholds) &&
      (heapSize.bytes() >= tunables.gcMaxBytes())) {
    return nullptr;
  }

  Arena* arena = chunk->allocateArena(this, zone, thingKind, lock);
  zone->gcHeapSize.addGCArena(heapSize);

  // Trigger an incremental slice if needed.
  if (checkThresholds != ShouldCheckThresholds::DontCheckThresholds) {
    maybeTriggerGCAfterAlloc(zone);
  }

  return arena;
}

Arena* TenuredChunk::allocateArena(GCRuntime* gc, Zone* zone,
                                   AllocKind thingKind,
                                   const AutoLockGC& lock) {
  if (info.numArenasFreeCommitted == 0) {
    commitOnePage(gc);
    MOZ_ASSERT(info.numArenasFreeCommitted == ArenasPerPage);
  }

  MOZ_ASSERT(info.numArenasFreeCommitted > 0);
  Arena* arena = fetchNextFreeArena(gc);

  arena->init(zone, thingKind, lock);
  updateChunkListAfterAlloc(gc, lock);

  verify();

  return arena;
}

template <size_t N>
static inline size_t FindFirstBitSet(
    const mozilla::BitSet<N, uint32_t>& bitset) {
  MOZ_ASSERT(!bitset.IsEmpty());

  const auto& words = bitset.Storage();
  for (size_t i = 0; i < words.Length(); i++) {
    uint32_t word = words[i];
    if (word) {
      return i * 32 + mozilla::CountTrailingZeroes32(word);
    }
  }

  MOZ_CRASH("No bits found");
}

void TenuredChunk::commitOnePage(GCRuntime* gc) {
  MOZ_ASSERT(info.numArenasFreeCommitted == 0);
  MOZ_ASSERT(info.numArenasFree >= ArenasPerPage);

  uint32_t pageIndex = FindFirstBitSet(decommittedPages);
  MOZ_ASSERT(decommittedPages[pageIndex]);

  if (DecommitEnabled()) {
    MarkPagesInUseSoft(pageAddress(pageIndex), PageSize);
  }

  decommittedPages[pageIndex] = false;

  for (size_t i = 0; i < ArenasPerPage; i++) {
    size_t arenaIndex = pageIndex * ArenasPerPage + i;
    MOZ_ASSERT(!freeCommittedArenas[arenaIndex]);
    freeCommittedArenas[arenaIndex] = true;
    arenas[arenaIndex].setAsNotAllocated();
    ++info.numArenasFreeCommitted;
    gc->updateOnArenaFree();
  }

  verify();
}

inline void GCRuntime::updateOnFreeArenaAlloc(const TenuredChunkInfo& info) {
  MOZ_ASSERT(info.numArenasFreeCommitted <= numArenasFreeCommitted);
  --numArenasFreeCommitted;
}

Arena* TenuredChunk::fetchNextFreeArena(GCRuntime* gc) {
  MOZ_ASSERT(info.numArenasFreeCommitted > 0);
  MOZ_ASSERT(info.numArenasFreeCommitted <= info.numArenasFree);

  size_t index = FindFirstBitSet(freeCommittedArenas);
  MOZ_ASSERT(freeCommittedArenas[index]);

  freeCommittedArenas[index] = false;
  --info.numArenasFreeCommitted;
  --info.numArenasFree;
  gc->updateOnFreeArenaAlloc(info);

  return &arenas[index];
}

// ///////////  System -> TenuredChunk Allocator  //////////////////////////////

TenuredChunk* GCRuntime::getOrAllocChunk(AutoLockGCBgAlloc& lock) {
  TenuredChunk* chunk = emptyChunks(lock).pop();
  if (chunk) {
    // Reinitialize ChunkBase; arenas are all free and may or may not be
    // committed.
    SetMemCheckKind(chunk, sizeof(ChunkBase), MemCheckKind::MakeUndefined);
    chunk->initBase(rt, nullptr);
    MOZ_ASSERT(chunk->unused());
  } else {
    void* ptr = TenuredChunk::allocate(this);
    if (!ptr) {
      return nullptr;
    }

    chunk = TenuredChunk::emplace(ptr, this, /* allMemoryCommitted = */ true);
    MOZ_ASSERT(chunk->info.numArenasFreeCommitted == 0);
  }

  if (wantBackgroundAllocation(lock)) {
    lock.tryToStartBackgroundAllocation();
  }

  return chunk;
}

void GCRuntime::recycleChunk(TenuredChunk* chunk, const AutoLockGC& lock) {
#ifdef DEBUG
  MOZ_ASSERT(chunk->unused());
  chunk->verify();
#endif

  // Poison ChunkBase to catch use after free.
  AlwaysPoison(chunk, JS_FREED_CHUNK_PATTERN, sizeof(ChunkBase),
               MemCheckKind::MakeNoAccess);

  emptyChunks(lock).push(chunk);
}

TenuredChunk* GCRuntime::pickChunk(AutoLockGCBgAlloc& lock) {
  if (availableChunks(lock).count()) {
    return availableChunks(lock).head();
  }

  TenuredChunk* chunk = getOrAllocChunk(lock);
  if (!chunk) {
    return nullptr;
  }

#ifdef DEBUG
  chunk->verify();
  MOZ_ASSERT(chunk->unused());
  MOZ_ASSERT(!fullChunks(lock).contains(chunk));
  MOZ_ASSERT(!availableChunks(lock).contains(chunk));
#endif

  availableChunks(lock).push(chunk);

  return chunk;
}

BackgroundAllocTask::BackgroundAllocTask(GCRuntime* gc, ChunkPool& pool)
    : GCParallelTask(gc, gcstats::PhaseKind::NONE),
      chunkPool_(pool),
      enabled_(CanUseExtraThreads() && GetCPUCount() >= 2) {
  // This can occur outside GCs so doesn't have a stats phase.
}

void BackgroundAllocTask::run(AutoLockHelperThreadState& lock) {
  AutoUnlockHelperThreadState unlock(lock);

  AutoLockGC gcLock(gc);
  while (!isCancelled() && gc->wantBackgroundAllocation(gcLock)) {
    TenuredChunk* chunk;
    {
      AutoUnlockGC unlock(gcLock);
      void* ptr = TenuredChunk::allocate(gc);
      if (!ptr) {
        break;
      }
      chunk = TenuredChunk::emplace(ptr, gc, /* allMemoryCommitted = */ true);
    }
    chunkPool_.ref().push(chunk);
  }
}

/* static */
void* TenuredChunk::allocate(GCRuntime* gc) {
  void* chunk = MapAlignedPages(ChunkSize, ChunkSize);
  if (!chunk) {
    return nullptr;
  }

  gc->stats().count(gcstats::COUNT_NEW_CHUNK);
  return chunk;
}

static inline bool ShouldDecommitNewChunk(bool allMemoryCommitted,
                                          const GCSchedulingState& state) {
  if (!DecommitEnabled()) {
    return false;
  }

  return !allMemoryCommitted || !state.inHighFrequencyGCMode();
}

TenuredChunk* TenuredChunk::emplace(void* ptr, GCRuntime* gc,
                                    bool allMemoryCommitted) {
  /* The chunk may still have some regions marked as no-access. */
  MOZ_MAKE_MEM_UNDEFINED(ptr, ChunkSize);

  /*
   * Poison the chunk. Note that decommitAllArenas() below will mark the
   * arenas as inaccessible (for memory sanitizers).
   */
  Poison(ptr, JS_FRESH_TENURED_PATTERN, ChunkSize, MemCheckKind::MakeUndefined);

  TenuredChunk* chunk = new (mozilla::KnownNotNull, ptr) TenuredChunk(gc->rt);

  if (ShouldDecommitNewChunk(allMemoryCommitted, gc->schedulingState)) {
    // Decommit the arenas. We do this after poisoning so that if the OS does
    // not have to recycle the pages, we still get the benefit of poisoning.
    chunk->decommitAllArenas();
  } else {
    // The chunk metadata is initialized as decommitted regardless, to avoid
    // having to initialize the arenas at this time.
    chunk->initAsDecommitted();
  }

  chunk->verify();

  return chunk;
}

void TenuredChunk::decommitAllArenas() {
  MOZ_ASSERT(unused());
  MarkPagesUnusedSoft(&arenas[0], ArenasPerChunk * ArenaSize);
  initAsDecommitted();
}

void TenuredChunkBase::initAsDecommitted() {
  // Set the state of all arenas to free and decommitted. They might not
  // actually be decommitted, but in that case the re-commit operation is a
  // no-op so it doesn't matter.
  decommittedPages.SetAll();
  freeCommittedArenas.ResetAll();
  info.numArenasFree = ArenasPerChunk;
  info.numArenasFreeCommitted = 0;
}
