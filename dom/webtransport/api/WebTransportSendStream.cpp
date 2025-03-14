/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/WebTransportSendStream.h"

#include "mozilla/dom/UnderlyingSinkCallbackHelpers.h"
#include "mozilla/dom/WritableStream.h"
#include "mozilla/dom/WebTransport.h"
#include "mozilla/dom/WebTransportSendReceiveStreamBinding.h"
#include "mozilla/ipc/DataPipe.h"

using namespace mozilla::ipc;

namespace mozilla::dom {

WebTransportSendStream::WebTransportSendStream(nsIGlobalObject* aGlobal)
    : WritableStream(aGlobal,
                     WritableStream::HoldDropJSObjectsCaller::Implicit) {}

JSObject* WebTransportSendStream::WrapObject(
    JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return WebTransportSendStream_Binding::Wrap(aCx, this, aGivenProto);
}

// NOTE: this does not yet implement SendOrder; see bug 1816925
/* static */
already_AddRefed<WebTransportSendStream> WebTransportSendStream::Create(
    WebTransport* aWebTransport, nsIGlobalObject* aGlobal,
    DataPipeSender* sender, ErrorResult& aRv) {
  // https://w3c.github.io/webtransport/#webtransportsendstream-create
  AutoJSAPI jsapi;
  if (!jsapi.Init(aGlobal)) {
    return nullptr;
  }
  JSContext* cx = jsapi.cx();

  auto stream = MakeRefPtr<WebTransportSendStream>(aGlobal);

  nsCOMPtr<nsIAsyncOutputStream> outputStream = sender;
  auto algorithms = MakeRefPtr<WritableStreamToOutput>(
      stream->GetParentObject(), outputStream);

  // Steps 2-5
  RefPtr<QueuingStrategySize> writableSizeAlgorithm;
  stream->SetUpNative(cx, *algorithms, Nothing(), writableSizeAlgorithm, aRv);

  // Step 6:  Add the following steps to stream’s [[controller]]'s [[signal]].
  // Step 6.1: If stream.[[PendingOperation]] is null, then abort these steps.
  // Step 6.2: Let reason be stream’s [[controller]]'s [[signal]]'s abort
  // reason. Step 6.3: Let abortPromise be the result of aborting stream with
  // reason. Step 6.4: Upon fulfillment of abortPromise, reject promise with
  // reason. Step 6.5: Let pendingOperation be stream.[[PendingOperation]].
  // Step 6.6: Set stream.[[PendingOperation]] to null.
  // Step 6.7: Resolve pendingOperation with promise.
  // XXX TODO

  // Step 7: Append stream to SendStreams
  aWebTransport->mSendStreams.AppendElement(stream);
  // Step 8: return stream
  return stream.forget();
}

already_AddRefed<Promise> WebTransportSendStream::GetStats() {
  RefPtr<Promise> promise = Promise::CreateInfallible(WritableStream::mGlobal);
  promise->MaybeRejectWithNotSupportedError("GetStats isn't supported yet");
  return promise.forget();
}

}  // namespace mozilla::dom
