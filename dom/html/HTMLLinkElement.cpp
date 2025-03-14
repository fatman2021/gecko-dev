/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/HTMLLinkElement.h"

#include "mozilla/AsyncEventDispatcher.h"
#include "mozilla/Attributes.h"
#include "mozilla/Components.h"
#include "mozilla/EventDispatcher.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/dom/BindContext.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/dom/HTMLLinkElementBinding.h"
#include "mozilla/dom/HTMLDNSPrefetch.h"
#include "mozilla/dom/ReferrerInfo.h"
#include "mozilla/dom/ScriptLoader.h"
#include "nsContentUtils.h"
#include "nsDOMTokenList.h"
#include "nsGenericHTMLElement.h"
#include "nsGkAtoms.h"
#include "nsIContentInlines.h"
#include "mozilla/dom/Document.h"
#include "nsINode.h"
#include "nsIPrefetchService.h"
#include "nsISizeOf.h"
#include "nsPIDOMWindow.h"
#include "nsReadableUtils.h"
#include "nsStyleConsts.h"
#include "nsUnicharUtils.h"
#include "nsWindowSizes.h"
#include "nsIContentPolicy.h"
#include "nsMimeTypes.h"
#include "imgLoader.h"
#include "MediaContainerType.h"
#include "DecoderDoctorDiagnostics.h"
#include "DecoderTraits.h"
#include "MediaList.h"
#include "nsAttrValueInlines.h"

NS_IMPL_NS_NEW_HTML_ELEMENT(Link)

namespace mozilla::dom {

HTMLLinkElement::HTMLLinkElement(
    already_AddRefed<mozilla::dom::NodeInfo>&& aNodeInfo)
    : nsGenericHTMLElement(std::move(aNodeInfo)) {}

HTMLLinkElement::~HTMLLinkElement() { SupportsDNSPrefetch::Destroyed(*this); }

NS_IMPL_CYCLE_COLLECTION_CLASS(HTMLLinkElement)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(HTMLLinkElement,
                                                  nsGenericHTMLElement)
  tmp->LinkStyle::Traverse(cb);
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mRelList)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mSizes)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(HTMLLinkElement,
                                                nsGenericHTMLElement)
  tmp->LinkStyle::Unlink();
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mRelList)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mSizes)
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(HTMLLinkElement,
                                               nsGenericHTMLElement)

NS_IMPL_ELEMENT_CLONE(HTMLLinkElement)

bool HTMLLinkElement::Disabled() const {
  return GetBoolAttr(nsGkAtoms::disabled);
}

void HTMLLinkElement::SetDisabled(bool aDisabled, ErrorResult& aRv) {
  return SetHTMLBoolAttr(nsGkAtoms::disabled, aDisabled, aRv);
}

nsresult HTMLLinkElement::BindToTree(BindContext& aContext, nsINode& aParent) {
  nsresult rv = nsGenericHTMLElement::BindToTree(aContext, aParent);
  NS_ENSURE_SUCCESS(rv, rv);

  if (IsInComposedDoc()) {
    TryDNSPrefetchOrPreconnectOrPrefetchOrPreloadOrPrerender();
  }

  void (HTMLLinkElement::*update)() =
      &HTMLLinkElement::UpdateStyleSheetInternal;
  nsContentUtils::AddScriptRunner(
      NewRunnableMethod("dom::HTMLLinkElement::BindToTree", this, update));

  if (IsInUncomposedDoc() &&
      AttrValueIs(kNameSpaceID_None, nsGkAtoms::rel, nsGkAtoms::localization,
                  eIgnoreCase)) {
    aContext.OwnerDoc().LocalizationLinkAdded(this);
  }

  LinkAdded();

  return rv;
}

void HTMLLinkElement::LinkAdded() {
  CreateAndDispatchEvent(OwnerDoc(), u"DOMLinkAdded"_ns);
}

void HTMLLinkElement::LinkRemoved() {
  CreateAndDispatchEvent(OwnerDoc(), u"DOMLinkRemoved"_ns);
}

void HTMLLinkElement::UnbindFromTree(bool aNullParent) {
  CancelDNSPrefetch(*this);
  CancelPrefetchOrPreload();

  // If this is reinserted back into the document it will not be
  // from the parser.
  Document* oldDoc = GetUncomposedDoc();
  ShadowRoot* oldShadowRoot = GetContainingShadow();

  // We want to update the localization but only if the link is removed from a
  // DOM change, and not because the document is going away.
  bool ignore;
  if (oldDoc && oldDoc->GetScriptHandlingObject(ignore) &&
      AttrValueIs(kNameSpaceID_None, nsGkAtoms::rel, nsGkAtoms::localization,
                  eIgnoreCase)) {
    oldDoc->LocalizationLinkRemoved(this);
  }

  CreateAndDispatchEvent(oldDoc, u"DOMLinkRemoved"_ns);
  nsGenericHTMLElement::UnbindFromTree(aNullParent);

  Unused << UpdateStyleSheetInternal(oldDoc, oldShadowRoot);
}

bool HTMLLinkElement::ParseAttribute(int32_t aNamespaceID, nsAtom* aAttribute,
                                     const nsAString& aValue,
                                     nsIPrincipal* aMaybeScriptedPrincipal,
                                     nsAttrValue& aResult) {
  if (aNamespaceID == kNameSpaceID_None) {
    if (aAttribute == nsGkAtoms::crossorigin) {
      ParseCORSValue(aValue, aResult);
      return true;
    }

    if (aAttribute == nsGkAtoms::as) {
      net::ParseAsValue(aValue, aResult);
      return true;
    }

    if (aAttribute == nsGkAtoms::sizes) {
      aResult.ParseAtomArray(aValue);
      return true;
    }

    if (aAttribute == nsGkAtoms::integrity) {
      aResult.ParseStringOrAtom(aValue);
      return true;
    }
  }

  return nsGenericHTMLElement::ParseAttribute(aNamespaceID, aAttribute, aValue,
                                              aMaybeScriptedPrincipal, aResult);
}

void HTMLLinkElement::CreateAndDispatchEvent(Document* aDoc,
                                             const nsAString& aEventName) {
  if (!aDoc) return;

  // In the unlikely case that both rev is specified *and* rel=stylesheet,
  // this code will cause the event to fire, on the principle that maybe the
  // page really does want to specify that its author is a stylesheet. Since
  // this should never actually happen and the performance hit is minimal,
  // doing the "right" thing costs virtually nothing here, even if it doesn't
  // make much sense.
  static AttrArray::AttrValuesArray strings[] = {
      nsGkAtoms::_empty, nsGkAtoms::stylesheet, nullptr};

  if (!nsContentUtils::HasNonEmptyAttr(this, kNameSpaceID_None,
                                       nsGkAtoms::rev) &&
      FindAttrValueIn(kNameSpaceID_None, nsGkAtoms::rel, strings,
                      eIgnoreCase) != AttrArray::ATTR_VALUE_NO_MATCH) {
    return;
  }

  RefPtr<AsyncEventDispatcher> asyncDispatcher = new AsyncEventDispatcher(
      this, aEventName, CanBubble::eYes, ChromeOnlyDispatch::eYes);
  // Always run async in order to avoid running script when the content
  // sink isn't expecting it.
  asyncDispatcher->PostDOMEvent();
}

nsresult HTMLLinkElement::BeforeSetAttr(int32_t aNameSpaceID, nsAtom* aName,
                                        const nsAttrValueOrString* aValue,
                                        bool aNotify) {
  if (aNameSpaceID == kNameSpaceID_None &&
      (aName == nsGkAtoms::href || aName == nsGkAtoms::rel)) {
    CancelDNSPrefetch(*this);
    CancelPrefetchOrPreload();
  }

  return nsGenericHTMLElement::BeforeSetAttr(aNameSpaceID, aName, aValue,
                                             aNotify);
}

nsresult HTMLLinkElement::AfterSetAttr(int32_t aNameSpaceID, nsAtom* aName,
                                       const nsAttrValue* aValue,
                                       const nsAttrValue* aOldValue,
                                       nsIPrincipal* aSubjectPrincipal,
                                       bool aNotify) {
  if (aNameSpaceID == kNameSpaceID_None && aName == nsGkAtoms::href) {
    mCachedURI = nullptr;
    if (IsInUncomposedDoc()) {
      CreateAndDispatchEvent(OwnerDoc(), u"DOMLinkChanged"_ns);
    }
    mTriggeringPrincipal = nsContentUtils::GetAttrTriggeringPrincipal(
        this, aValue ? aValue->GetStringValue() : EmptyString(),
        aSubjectPrincipal);

    // If the link has `rel=localization` and its `href` attribute is changed,
    // update the list of localization links.
    if (AttrValueIs(kNameSpaceID_None, nsGkAtoms::rel, nsGkAtoms::localization,
                    eIgnoreCase)) {
      if (Document* doc = GetUncomposedDoc()) {
        if (aOldValue) {
          doc->LocalizationLinkRemoved(this);
        }
        if (aValue) {
          doc->LocalizationLinkAdded(this);
        }
      }
    }
  }

  // If a link's `rel` attribute was changed from or to `localization`,
  // update the list of localization links.
  if (aNameSpaceID == kNameSpaceID_None && aName == nsGkAtoms::rel) {
    if (Document* doc = GetUncomposedDoc()) {
      if ((aValue && aValue->Equals(nsGkAtoms::localization, eIgnoreCase)) &&
          (!aOldValue ||
           !aOldValue->Equals(nsGkAtoms::localization, eIgnoreCase))) {
        doc->LocalizationLinkAdded(this);
      } else if ((aOldValue &&
                  aOldValue->Equals(nsGkAtoms::localization, eIgnoreCase)) &&
                 (!aValue ||
                  !aValue->Equals(nsGkAtoms::localization, eIgnoreCase))) {
        doc->LocalizationLinkRemoved(this);
      }
    }
  }

  if (aValue) {
    if (aNameSpaceID == kNameSpaceID_None &&
        (aName == nsGkAtoms::href || aName == nsGkAtoms::rel ||
         aName == nsGkAtoms::title || aName == nsGkAtoms::media ||
         aName == nsGkAtoms::type || aName == nsGkAtoms::as ||
         aName == nsGkAtoms::crossorigin || aName == nsGkAtoms::disabled)) {
      bool dropSheet = false;
      if (aName == nsGkAtoms::rel) {
        nsAutoString value;
        aValue->ToString(value);
        uint32_t linkTypes = ParseLinkTypes(value);
        if (GetSheet()) {
          dropSheet = !(linkTypes & eSTYLESHEET);
        }
      }

      if ((aName == nsGkAtoms::rel || aName == nsGkAtoms::href) &&
          IsInComposedDoc()) {
        TryDNSPrefetchOrPreconnectOrPrefetchOrPreloadOrPrerender();
      }

      if ((aName == nsGkAtoms::as || aName == nsGkAtoms::type ||
           aName == nsGkAtoms::crossorigin || aName == nsGkAtoms::media) &&
          IsInComposedDoc()) {
        UpdatePreload(aName, aValue, aOldValue);
      }

      const bool forceUpdate =
          dropSheet || aName == nsGkAtoms::title || aName == nsGkAtoms::media ||
          aName == nsGkAtoms::type || aName == nsGkAtoms::disabled;

      Unused << UpdateStyleSheetInternal(
          nullptr, nullptr, forceUpdate ? ForceUpdate::Yes : ForceUpdate::No);
    }
  } else {
    if (aNameSpaceID == kNameSpaceID_None) {
      if (aName == nsGkAtoms::disabled) {
        mExplicitlyEnabled = true;
      }
      // Since removing href or rel makes us no longer link to a stylesheet,
      // force updates for those too.
      if (aName == nsGkAtoms::href || aName == nsGkAtoms::rel ||
          aName == nsGkAtoms::title || aName == nsGkAtoms::media ||
          aName == nsGkAtoms::type || aName == nsGkAtoms::disabled) {
        Unused << UpdateStyleSheetInternal(nullptr, nullptr, ForceUpdate::Yes);
      }
      if ((aName == nsGkAtoms::as || aName == nsGkAtoms::type ||
           aName == nsGkAtoms::crossorigin || aName == nsGkAtoms::media) &&
          IsInComposedDoc()) {
        UpdatePreload(aName, aValue, aOldValue);
      }
    }
  }

  return nsGenericHTMLElement::AfterSetAttr(
      aNameSpaceID, aName, aValue, aOldValue, aSubjectPrincipal, aNotify);
}

static const DOMTokenListSupportedToken sSupportedRelValues[] = {
    // Keep this and the one below in sync with ToLinkMask in
    // LinkStyle.cpp.
    // "preload" must come first because it can be disabled.
    "preload", "prefetch",      "dns-prefetch", "stylesheet",
    "next",    "alternate",     "preconnect",   "icon",
    "search",  "modulepreload", nullptr};

static const DOMTokenListSupportedToken sSupportedRelValuesWithManifest[] = {
    // Keep this in sync with ToLinkMask in LinkStyle.cpp.
    // "preload" and "manifest" must come first because they can be disabled.
    "preload",   "manifest",   "prefetch", "dns-prefetch", "stylesheet", "next",
    "alternate", "preconnect", "icon",     "search",       nullptr};

nsDOMTokenList* HTMLLinkElement::RelList() {
  if (!mRelList) {
    auto preload = StaticPrefs::network_preload();
    auto manifest = StaticPrefs::dom_manifest_enabled();
    if (manifest && preload) {
      mRelList = new nsDOMTokenList(this, nsGkAtoms::rel,
                                    sSupportedRelValuesWithManifest);
    } else if (manifest && !preload) {
      mRelList = new nsDOMTokenList(this, nsGkAtoms::rel,
                                    &sSupportedRelValuesWithManifest[1]);
    } else if (!manifest && preload) {
      mRelList = new nsDOMTokenList(this, nsGkAtoms::rel, sSupportedRelValues);
    } else {  // both false...drop preload
      mRelList =
          new nsDOMTokenList(this, nsGkAtoms::rel, &sSupportedRelValues[1]);
    }
  }
  return mRelList;
}

Maybe<LinkStyle::SheetInfo> HTMLLinkElement::GetStyleSheetInfo() {
  nsAutoString rel;
  GetAttr(kNameSpaceID_None, nsGkAtoms::rel, rel);
  uint32_t linkTypes = ParseLinkTypes(rel);
  if (!(linkTypes & eSTYLESHEET)) {
    return Nothing();
  }

  if (!IsCSSMimeTypeAttributeForLinkElement(*this)) {
    return Nothing();
  }

  if (Disabled()) {
    return Nothing();
  }

  nsAutoString title;
  nsAutoString media;
  GetTitleAndMediaForElement(*this, title, media);

  bool alternate = linkTypes & eALTERNATE;
  if (alternate && title.IsEmpty()) {
    // alternates must have title.
    return Nothing();
  }

  if (!HasNonEmptyAttr(nsGkAtoms::href)) {
    return Nothing();
  }

  nsAutoString integrity;
  GetAttr(nsGkAtoms::integrity, integrity);

  nsCOMPtr<nsIURI> uri = GetURI();
  nsCOMPtr<nsIPrincipal> prin = mTriggeringPrincipal;

  nsAutoString nonce;
  nsString* cspNonce = static_cast<nsString*>(GetProperty(nsGkAtoms::nonce));
  if (cspNonce) {
    nonce = *cspNonce;
  }

  return Some(SheetInfo{
      *OwnerDoc(),
      this,
      uri.forget(),
      prin.forget(),
      MakeAndAddRef<ReferrerInfo>(*this),
      GetCORSMode(),
      title,
      media,
      integrity,
      nonce,
      alternate ? HasAlternateRel::Yes : HasAlternateRel::No,
      IsInline::No,
      mExplicitlyEnabled ? IsExplicitlyEnabled::Yes : IsExplicitlyEnabled::No,
  });
}

void HTMLLinkElement::AddSizeOfExcludingThis(nsWindowSizes& aSizes,
                                             size_t* aNodeSize) const {
  nsGenericHTMLElement::AddSizeOfExcludingThis(aSizes, aNodeSize);
  if (nsCOMPtr<nsISizeOf> iface = do_QueryInterface(mCachedURI)) {
    *aNodeSize += iface->SizeOfExcludingThis(aSizes.mState.mMallocSizeOf);
  }
}

JSObject* HTMLLinkElement::WrapNode(JSContext* aCx,
                                    JS::Handle<JSObject*> aGivenProto) {
  return HTMLLinkElement_Binding::Wrap(aCx, this, aGivenProto);
}

void HTMLLinkElement::GetAs(nsAString& aResult) {
  GetEnumAttr(nsGkAtoms::as, "", aResult);
}

void HTMLLinkElement::GetContentPolicyMimeTypeMedia(
    nsAttrValue& aAsAttr, nsContentPolicyType& aPolicyType, nsString& aMimeType,
    nsAString& aMedia) {
  nsAutoString as;
  GetAttr(kNameSpaceID_None, nsGkAtoms::as, as);
  net::ParseAsValue(as, aAsAttr);
  aPolicyType = net::AsValueToContentPolicy(aAsAttr);

  nsAutoString type;
  GetAttr(kNameSpaceID_None, nsGkAtoms::type, type);
  nsAutoString notUsed;
  nsContentUtils::SplitMimeType(type, aMimeType, notUsed);

  GetAttr(kNameSpaceID_None, nsGkAtoms::media, aMedia);
}

void HTMLLinkElement::
    TryDNSPrefetchOrPreconnectOrPrefetchOrPreloadOrPrerender() {
  MOZ_ASSERT(IsInComposedDoc());
  if (!HasAttr(nsGkAtoms::href)) {
    return;
  }

  nsAutoString rel;
  if (!GetAttr(nsGkAtoms::rel, rel)) {
    return;
  }

  if (!nsContentUtils::PrefetchPreloadEnabled(OwnerDoc()->GetDocShell())) {
    return;
  }

  uint32_t linkTypes = ParseLinkTypes(rel);

  if ((linkTypes & ePREFETCH) || (linkTypes & eNEXT)) {
    nsCOMPtr<nsIPrefetchService> prefetchService(
        components::Prefetch::Service());
    if (prefetchService) {
      if (nsCOMPtr<nsIURI> uri = GetURI()) {
        auto referrerInfo = MakeRefPtr<ReferrerInfo>(*this);
        prefetchService->PrefetchURI(uri, referrerInfo, this,
                                     linkTypes & ePREFETCH);
        return;
      }
    }
  }

  if (linkTypes & ePRELOAD) {
    if (nsCOMPtr<nsIURI> uri = GetURI()) {
      nsContentPolicyType policyType;

      nsAttrValue asAttr;
      nsAutoString mimeType;
      nsAutoString media;
      GetContentPolicyMimeTypeMedia(asAttr, policyType, mimeType, media);

      if (policyType == nsIContentPolicy::TYPE_INVALID ||
          !net::CheckPreloadAttrs(asAttr, mimeType, media, OwnerDoc())) {
        // Ignore preload with a wrong or empty as attribute.
        net::WarnIgnoredPreload(*OwnerDoc(), *uri);
        return;
      }

      StartPreload(policyType);
      return;
    }
  }

  if (linkTypes & eMODULE_PRELOAD) {
    if (!OwnerDoc()->ScriptLoader()->GetModuleLoader()) {
      // For the print preview documents, at this moment it doesn't have module
      // loader yet, as the (print preview) document is not attached to the
      // nsIContentViewer yet, so it doesn't have the GlobalObject.
      // Also, the script elements won't be processed as they are also cloned
      // from the original document.
      // So we simply bail out if the module loader is null.
      return;
    }

    // https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-modulepreload-module-script-graph
    // Step 1. Disallow further import maps given settings object.
    OwnerDoc()->ScriptLoader()->GetModuleLoader()->DisallowImportMaps();
    return;
  }

  if (linkTypes & ePRECONNECT) {
    if (nsCOMPtr<nsIURI> uri = GetURI()) {
      OwnerDoc()->MaybePreconnect(
          uri, AttrValueToCORSMode(GetParsedAttr(nsGkAtoms::crossorigin)));
      return;
    }
  }

  if (linkTypes & eDNS_PREFETCH) {
    TryDNSPrefetch(*this);
  }
}

void HTMLLinkElement::UpdatePreload(nsAtom* aName, const nsAttrValue* aValue,
                                    const nsAttrValue* aOldValue) {
  MOZ_ASSERT(IsInComposedDoc());

  if (!HasAttr(nsGkAtoms::href)) {
    return;
  }

  nsAutoString rel;
  if (!GetAttr(nsGkAtoms::rel, rel)) {
    return;
  }

  if (!nsContentUtils::PrefetchPreloadEnabled(OwnerDoc()->GetDocShell())) {
    return;
  }

  uint32_t linkTypes = ParseLinkTypes(rel);

  if (!(linkTypes & ePRELOAD)) {
    return;
  }

  nsCOMPtr<nsIURI> uri = GetURI();
  if (!uri) {
    return;
  }

  nsAttrValue asAttr;
  nsContentPolicyType asPolicyType;
  nsAutoString mimeType;
  nsAutoString media;
  GetContentPolicyMimeTypeMedia(asAttr, asPolicyType, mimeType, media);

  if (asPolicyType == nsIContentPolicy::TYPE_INVALID ||
      !net::CheckPreloadAttrs(asAttr, mimeType, media, OwnerDoc())) {
    // Ignore preload with a wrong or empty as attribute, but be sure to cancel
    // the old one.
    CancelPrefetchOrPreload();
    net::WarnIgnoredPreload(*OwnerDoc(), *uri);
    return;
  }

  if (aName == nsGkAtoms::crossorigin) {
    CORSMode corsMode = AttrValueToCORSMode(aValue);
    CORSMode oldCorsMode = AttrValueToCORSMode(aOldValue);
    if (corsMode != oldCorsMode) {
      CancelPrefetchOrPreload();
      StartPreload(asPolicyType);
    }
    return;
  }

  nsContentPolicyType oldPolicyType;

  if (aName == nsGkAtoms::as) {
    if (aOldValue) {
      oldPolicyType = net::AsValueToContentPolicy(*aOldValue);
      if (!net::CheckPreloadAttrs(*aOldValue, mimeType, media, OwnerDoc())) {
        oldPolicyType = nsIContentPolicy::TYPE_INVALID;
      }
    } else {
      oldPolicyType = nsIContentPolicy::TYPE_INVALID;
    }
  } else if (aName == nsGkAtoms::type) {
    nsAutoString oldType;
    nsAutoString notUsed;
    if (aOldValue) {
      aOldValue->ToString(oldType);
    }
    nsAutoString oldMimeType;
    nsContentUtils::SplitMimeType(oldType, oldMimeType, notUsed);
    if (net::CheckPreloadAttrs(asAttr, oldMimeType, media, OwnerDoc())) {
      oldPolicyType = asPolicyType;
    } else {
      oldPolicyType = nsIContentPolicy::TYPE_INVALID;
    }
  } else {
    MOZ_ASSERT(aName == nsGkAtoms::media);
    nsAutoString oldMedia;
    if (aOldValue) {
      aOldValue->ToString(oldMedia);
    }
    if (net::CheckPreloadAttrs(asAttr, mimeType, oldMedia, OwnerDoc())) {
      oldPolicyType = asPolicyType;
    } else {
      oldPolicyType = nsIContentPolicy::TYPE_INVALID;
    }
  }

  if (asPolicyType != oldPolicyType &&
      oldPolicyType != nsIContentPolicy::TYPE_INVALID) {
    CancelPrefetchOrPreload();
  }

  // Trigger a new preload if the policy type has changed.
  if (asPolicyType != oldPolicyType) {
    StartPreload(asPolicyType);
  }
}

void HTMLLinkElement::CancelPrefetchOrPreload() {
  CancelPreload();

  nsCOMPtr<nsIPrefetchService> prefetchService(components::Prefetch::Service());
  if (prefetchService) {
    if (nsCOMPtr<nsIURI> uri = GetURI()) {
      prefetchService->CancelPrefetchPreloadURI(uri, this);
    }
  }
}

void HTMLLinkElement::StartPreload(nsContentPolicyType aPolicyType) {
  MOZ_ASSERT(!mPreload, "Forgot to cancel the running preload");
  RefPtr<PreloaderBase> preload =
      OwnerDoc()->Preloads().PreloadLinkElement(this, aPolicyType);
  mPreload = preload.get();
}

void HTMLLinkElement::CancelPreload() {
  if (mPreload) {
    // This will cancel the loading channel if this was the last referred node
    // and the preload is not used up until now to satisfy a regular tag load
    // request.
    mPreload->RemoveLinkPreloadNode(this);
    mPreload = nullptr;
  }
}

bool HTMLLinkElement::IsCSSMimeTypeAttributeForLinkElement(
    const Element& aSelf) {
  // Processing the type attribute per
  // https://html.spec.whatwg.org/multipage/semantics.html#processing-the-type-attribute
  // for HTML link elements.
  nsAutoString type;
  nsAutoString mimeType;
  nsAutoString notUsed;
  aSelf.GetAttr(kNameSpaceID_None, nsGkAtoms::type, type);
  nsContentUtils::SplitMimeType(type, mimeType, notUsed);
  return mimeType.IsEmpty() || mimeType.LowerCaseEqualsLiteral("text/css");
}

}  // namespace mozilla::dom
