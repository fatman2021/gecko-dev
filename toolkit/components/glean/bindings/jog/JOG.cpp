/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/glean/bindings/jog/JOG.h"

#include <locale>

#include "mozilla/ClearOnShutdown.h"
#include "mozilla/DataMutex.h"
#include "mozilla/glean/bindings/jog/jog_ffi_generated.h"
#include "mozilla/Logging.h"
#include "mozilla/Omnijar.h"
#include "mozilla/Tuple.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsThreadUtils.h"
#include "nsTHashMap.h"
#include "nsTHashSet.h"

namespace mozilla::glean {

using mozilla::LogLevel;
static mozilla::LazyLogModule sLog("jog");

// Storage
// Thread Safety: Only used on the main thread.
StaticAutoPtr<nsTHashSet<nsCString>> gCategories;
StaticAutoPtr<nsTHashMap<nsCString, uint32_t>> gMetrics;
StaticAutoPtr<nsTHashMap<nsCString, uint32_t>> gPings;

// static
bool JOG::HasCategory(const nsACString& aCategoryName) {
  MOZ_ASSERT(NS_IsMainThread());

  return gCategories && gCategories->Contains(aCategoryName);
}

static Maybe<bool> sFoundAndLoadedJogfile;

// static
bool JOG::EnsureRuntimeMetricsRegistered(bool aForce) {
  MOZ_ASSERT(NS_IsMainThread());

#ifdef MOZILLA_OFFICIAL
  // In the event we're an official build we want there to be no chance we might
  // accidentally perform I/O on the main thread.
  MOZ_LOG(sLog, LogLevel::Verbose, ("MOZILLA_OFFICIAL build. No JOG for you."));
  return false;
#endif

  if (sFoundAndLoadedJogfile) {
    return sFoundAndLoadedJogfile.value();
  }
  sFoundAndLoadedJogfile = Some(false);

  MOZ_LOG(sLog, LogLevel::Debug, ("Determining whether there's JOG for you."));

  if (mozilla::IsPackagedBuild()) {
    // Supporting Artifact Builds is a developer-only thing.
    // We're on the main thread here.
    // Let's not spend any more time than we need to.
    MOZ_LOG(sLog, LogLevel::Debug, ("IsPackagedBuild. No JOG for you."));
    return false;
  }
  // The metrics we need to process were placed in GreD in jogfile.json
  // That file was generated by
  // toolkit/components/glean/build_scripts/glean_parser_ext/jog.py
  nsCOMPtr<nsIFile> jogfile;
  if (NS_WARN_IF(NS_FAILED(
          NS_GetSpecialDirectory(NS_GRE_DIR, getter_AddRefs(jogfile))))) {
    return false;
  }
  if (NS_WARN_IF(NS_FAILED(jogfile->Append(u"jogfile.json"_ns)))) {
    return false;
  }
  bool jogfileExists = false;
  if (NS_WARN_IF(NS_FAILED(jogfile->Exists(&jogfileExists))) ||
      !jogfileExists) {
    return false;
  }

  // We _could_ register everything here in C++ land,
  // but let's use Rust because (among other reasons) it's more fun.
  nsAutoString jogfileString;
  if (NS_WARN_IF(NS_FAILED(jogfile->GetPath(jogfileString)))) {
    return false;
  }
  sFoundAndLoadedJogfile = Some(jog::jog_load_jogfile(&jogfileString));
  MOZ_LOG(sLog, LogLevel::Debug,
          ("%s", sFoundAndLoadedJogfile.value()
                     ? "Found and loaded jogfile. Yes! JOG for you!"
                     : "Couldn't find and load jogfile. No JOG for you."));
  return sFoundAndLoadedJogfile.value();
}

// static
bool JOG::AreRuntimeMetricsComprehensive() {
  MOZ_ASSERT(NS_IsMainThread());
  return sFoundAndLoadedJogfile && sFoundAndLoadedJogfile.value();
}

// static
void JOG::GetCategoryNames(nsTArray<nsString>& aNames) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!gCategories) {
    return;
  }
  for (const auto& category : *gCategories) {
    aNames.EmplaceBack(NS_ConvertUTF8toUTF16(category));
  }
}

// static
Maybe<uint32_t> JOG::GetMetric(const nsACString& aMetricName) {
  MOZ_ASSERT(NS_IsMainThread());
  return !gMetrics ? Nothing() : gMetrics->MaybeGet(aMetricName);
}

// static
Maybe<uint32_t> JOG::GetPing(const nsACString& aPingName) {
  MOZ_ASSERT(NS_IsMainThread());
  return !gPings ? Nothing() : gPings->MaybeGet(aPingName);
}

}  // namespace mozilla::glean

// static
nsCString dottedSnakeToCamel(const nsACString& aSnake) {
  nsCString camel;
  bool first = true;
  for (const nsACString& segment : aSnake.Split('_')) {
    for (const nsACString& part : segment.Split('.')) {
      if (first) {
        first = false;
        camel.Append(part);
      } else if (part.Length()) {
        char lower = part.CharAt(0);
        if ('a' <= lower && lower <= 'z') {
          camel.Append(
              std::toupper(lower, std::locale()));  // append the Capital.
          camel.Append(part.BeginReading() + 1,
                       part.Length() - 1);  // append the rest.
        } else {
          // Not gonna try to capitalize anything outside a->z.
          camel.Append(part);
        }
      }
    }
  }
  return camel;
}

// static
nsCString kebabToCamel(const nsACString& aKebab) {
  nsCString camel;
  bool first = true;
  for (const nsACString& segment : aKebab.Split('-')) {
    if (first) {
      first = false;
      camel.Append(segment);
    } else if (segment.Length()) {
      char lower = segment.CharAt(0);
      if ('a' <= lower && lower <= 'z') {
        camel.Append(
            std::toupper(lower, std::locale()));  // append the Capital.
        camel.Append(segment.BeginReading() + 1,
                     segment.Length() - 1);  // append the rest.
      } else {
        // Not gonna try to capitalize anything outside a->z.
        camel.Append(segment);
      }
    }
  }
  return camel;
}

using mozilla::AppShutdown;
using mozilla::ShutdownPhase;
using mozilla::glean::gCategories;
using mozilla::glean::gMetrics;
using mozilla::glean::gPings;

extern "C" NS_EXPORT void JOG_RegisterMetric(const nsACString& aCategory,
                                             const nsACString& aName,
                                             uint32_t aMetric) {
  MOZ_ASSERT(NS_IsMainThread());

  if (AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMWillShutdown)) {
    return;
  }

  // aCategory is dotted.snake_case. aName is snake_case.
  auto categoryCamel = dottedSnakeToCamel(aCategory);
  auto nameCamel = dottedSnakeToCamel(aName);

  // Register the category
  if (!gCategories) {
    gCategories = new nsTHashSet<nsCString>();
    RunOnShutdown([&] { gCategories = nullptr; },
                  ShutdownPhase::XPCOMWillShutdown);
  }
  gCategories->Insert(categoryCamel);

  // Register the metric
  if (!gMetrics) {
    gMetrics = new nsTHashMap<nsCString, uint32_t>();
    RunOnShutdown([&] { gMetrics = nullptr; },
                  ShutdownPhase::XPCOMWillShutdown);
  }
  gMetrics->InsertOrUpdate(categoryCamel + "."_ns + nameCamel, aMetric);
}

extern "C" NS_EXPORT void JOG_RegisterPing(const nsACString& aPingName,
                                           uint32_t aPingId) {
  MOZ_ASSERT(NS_IsMainThread());

  if (AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMWillShutdown)) {
    return;
  }

  // aPingName is kebab-case. JS expects camelCase.
  auto pingCamel = kebabToCamel(aPingName);

  // Register the ping
  if (!gPings) {
    gPings = new nsTHashMap<nsCString, uint32_t>();
    RunOnShutdown([&] { gPings = nullptr; }, ShutdownPhase::XPCOMWillShutdown);
  }
  gPings->InsertOrUpdate(pingCamel, aPingId);
}
