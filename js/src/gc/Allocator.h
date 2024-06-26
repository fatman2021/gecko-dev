/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_Allocator_h
#define gc_Allocator_h

#include "mozilla/OperatorNewExtensions.h"

#include <stdint.h>

#include "gc/AllocKind.h"
#include "js/TypeDecls.h"

namespace js {

// [SMDOC] AllowGC template parameter
//
// AllowGC is a template parameter for functions that support both with and
// without GC operation.
//
// The CanGC variant of the function can trigger a garbage collection, and
// should set a pending exception on failure.
//
// The NoGC variant of the function cannot trigger a garbage collection, and
// should not set any pending exception on failure.  This variant can be called
// in fast paths where the caller has unrooted pointers.  The failure means we
// need to perform GC to allocate an object. The caller can fall back to a slow
// path that roots pointers before calling a CanGC variant of the function,
// without having to clear a pending exception.
enum AllowGC { NoGC = 0, CanGC = 1 };

namespace gc {
class AllocSite;
struct Cell;
class TenuredCell;

// `friend` js::gc::CellAllocator in a subtype T of Cell in order to allow it to
// be allocated with cx->newCell<T>(...). The friend declaration will allow
// calling T's constructor.
class CellAllocator {
  template <AllowGC allowGC = CanGC>
  static gc::Cell* AllocateStringCell(JSContext* cx, gc::AllocKind kind,
                                      size_t size, gc::InitialHeap heap);

  // Allocate a string. Use cx->newCell<StringT>([heap]).
  //
  // Use for nursery-allocatable strings. Returns a value cast to the correct
  // type. Non-nursery-allocatable strings will go through the fallback
  // tenured-only allocation path.
  template <typename StringT, AllowGC allowGC = CanGC, typename... Args>
  static StringT* AllocateString(JSContext* cx, gc::InitialHeap heap,
                                 Args&&... args) {
    static_assert(std::is_base_of_v<JSString, StringT>);
    gc::AllocKind kind = gc::MapTypeToAllocKind<StringT>::kind;
    gc::Cell* cell =
        AllocateStringCell<allowGC>(cx, kind, sizeof(StringT), heap);
    if (!cell) {
      return nullptr;
    }
    return new (mozilla::KnownNotNull, cell)
        StringT(std::forward<Args>(args)...);
  }

  // Use for nursery-allocatable BigInt.
  template <AllowGC allowGC = CanGC>
  static JS::BigInt* AllocateBigInt(JSContext* cx, gc::InitialHeap heap);

  // Allocate a JSObject. Use cx->newCell<ObjectT>(kind, ...).
  //
  // Parameters support various optimizations. If dynamic slots are requested
  // they will be allocated and the pointer stored directly in
  // |NativeObject::slots_|.
  template <AllowGC allowGC = CanGC>
  static JSObject* AllocateObject(JSContext* cx, gc::AllocKind kind,
                                  size_t nDynamicSlots, gc::InitialHeap heap,
                                  const JSClass* clasp,
                                  gc::AllocSite* site = nullptr);

 public:
  template <typename T, js::AllowGC allowGC = CanGC, typename... Args>
  static T* NewCell(JSContext* cx, Args&&... args);
};

namespace detail {

// Allocator implementation functions. SpiderMonkey code outside this file
// should use
//
//     cx->newCell<T>(...)
//
// or optionally
//
//     cx->newCell<T, AllowGC::NoGC>(...)
//
// The parameters will be passed to a type-specific function or constructor. For
// nursery-allocatable types, see eg AllocateString, AllocateObject, and
// AllocateBigInt in the CellAllocator class. For all other types, the
// parameters will be forwarded to the constructor.

template <AllowGC allowGC = CanGC>
gc::TenuredCell* AllocateTenuredCell(JSContext* cx, gc::AllocKind kind,
                                     size_t size);

}  // namespace detail
}  // namespace gc

// This is the entry point for all allocation, though callers should still not
// use this directly. Use cx->newCell<T>(...) instead.
//
// After a successful allocation the caller must fully initialize the thing
// before calling any function that can potentially trigger GC. This will
// ensure that GC tracing never sees junk values stored in the partially
// initialized thing.
template <typename T, AllowGC allowGC, typename... Args>
T* gc::CellAllocator::NewCell(JSContext* cx, Args&&... args) {
  static_assert(std::is_base_of_v<gc::Cell, T>);

  // Objects. See the valid parameter list in AllocateObject, above.
  if constexpr (std::is_base_of_v<JSObject, T>) {
    return static_cast<T*>(
        AllocateObject<allowGC>(cx, std::forward<Args>(args)...));
  }

  // BigInt
  else if constexpr (std::is_base_of_v<JS::BigInt, T>) {
    return AllocateBigInt<allowGC>(cx, std::forward<Args>(args)...);
  }

  // "Normal" strings (all of which can be nursery allocated). Atoms and
  // external strings will fall through to the generic code below. All other
  // strings go through AllocateString, which will forward the arguments to the
  // appropriate string class's constructor.
  else if constexpr (std::is_base_of_v<JSString, T> &&
                     !std::is_base_of_v<JSAtom, T> &&
                     !std::is_base_of_v<JSExternalString, T>) {
    return AllocateString<T, allowGC>(cx, std::forward<Args>(args)...);
  }

  else {
    // Allocate a new tenured GC thing that's not nursery-allocatable. Use
    // cx->newCell<T>(...), where the parameters are forwarded to the type's
    // constructor.
    gc::AllocKind kind = gc::MapTypeToAllocKind<T>::kind;
    gc::TenuredCell* cell =
        gc::detail::AllocateTenuredCell<allowGC>(cx, kind, sizeof(T));
    if (!cell) {
      return nullptr;
    }
    return new (mozilla::KnownNotNull, cell) T(std::forward<Args>(args)...);
  }
}

}  // namespace js

#endif  // gc_Allocator_h
