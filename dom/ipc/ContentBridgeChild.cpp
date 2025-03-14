/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/ContentBridgeChild.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/TabChild.h"
#include "mozilla/dom/ipc/BlobChild.h"
#include "mozilla/jsipc/CrossProcessObjectWrappers.h"
#include "mozilla/ipc/InputStreamUtils.h"
#include "base/task.h"

using namespace mozilla::ipc;
using namespace mozilla::jsipc;

namespace mozilla {
namespace dom {

NS_IMPL_ISUPPORTS(ContentBridgeChild,
                  nsIContentChild)

ContentBridgeChild::ContentBridgeChild()
{}

ContentBridgeChild::~ContentBridgeChild()
{
}

void
ContentBridgeChild::ActorDestroy(ActorDestroyReason aWhy)
{
  MessageLoop::current()->PostTask(NewRunnableMethod(this, &ContentBridgeChild::DeferredDestroy));
}

/*static*/ void
ContentBridgeChild::Create(Endpoint<PContentBridgeChild>&& aEndpoint)
{
  RefPtr<ContentBridgeChild> bridge = new ContentBridgeChild();
  bridge->mSelfRef = bridge;

  DebugOnly<bool> ok = aEndpoint.Bind(bridge);
  MOZ_ASSERT(ok);
}

void
ContentBridgeChild::DeferredDestroy()
{
  mSelfRef = nullptr;
  // |this| was just destroyed, hands off
}

mozilla::ipc::IPCResult
ContentBridgeChild::RecvAsyncMessage(const nsString& aMsg,
                                     InfallibleTArray<jsipc::CpowEntry>&& aCpows,
                                     const IPC::Principal& aPrincipal,
                                     const ClonedMessageData& aData)
{
  return nsIContentChild::RecvAsyncMessage(aMsg, Move(aCpows), aPrincipal, aData);
}

PBlobChild*
ContentBridgeChild::SendPBlobConstructor(PBlobChild* actor,
                                         const BlobConstructorParams& params)
{
  return PContentBridgeChild::SendPBlobConstructor(actor, params);
}

PMemoryStreamChild*
ContentBridgeChild::SendPMemoryStreamConstructor(const uint64_t& aSize)
{
  return PContentBridgeChild::SendPMemoryStreamConstructor(aSize);
}

bool
ContentBridgeChild::SendPBrowserConstructor(PBrowserChild* aActor,
                                            const TabId& aTabId,
                                            const IPCTabContext& aContext,
                                            const uint32_t& aChromeFlags,
                                            const ContentParentId& aCpID,
                                            const bool& aIsForBrowser)
{
  return PContentBridgeChild::SendPBrowserConstructor(aActor,
                                                      aTabId,
                                                      aContext,
                                                      aChromeFlags,
                                                      aCpID,
                                                      aIsForBrowser);
}

PFileDescriptorSetChild*
ContentBridgeChild::SendPFileDescriptorSetConstructor(const FileDescriptor& aFD)
{
  return PContentBridgeChild::SendPFileDescriptorSetConstructor(aFD);
}

PChildToParentStreamChild*
ContentBridgeChild::SendPChildToParentStreamConstructor(PChildToParentStreamChild* aActor)
{
  return PContentBridgeChild::SendPChildToParentStreamConstructor(aActor);
}

// This implementation is identical to ContentChild::GetCPOWManager but we can't
// move it to nsIContentChild because it calls ManagedPJavaScriptChild() which
// only exists in PContentChild and PContentBridgeChild.
jsipc::CPOWManager*
ContentBridgeChild::GetCPOWManager()
{
  if (PJavaScriptChild* c = LoneManagedOrNullAsserts(ManagedPJavaScriptChild())) {
    return CPOWManagerFor(c);
  }
  return CPOWManagerFor(SendPJavaScriptConstructor());
}

mozilla::jsipc::PJavaScriptChild *
ContentBridgeChild::AllocPJavaScriptChild()
{
  return nsIContentChild::AllocPJavaScriptChild();
}

bool
ContentBridgeChild::DeallocPJavaScriptChild(PJavaScriptChild *child)
{
  return nsIContentChild::DeallocPJavaScriptChild(child);
}

PBrowserChild*
ContentBridgeChild::AllocPBrowserChild(const TabId& aTabId,
                                       const IPCTabContext &aContext,
                                       const uint32_t& aChromeFlags,
                                       const ContentParentId& aCpID,
                                       const bool& aIsForBrowser)
{
  return nsIContentChild::AllocPBrowserChild(aTabId,
                                             aContext,
                                             aChromeFlags,
                                             aCpID,
                                             aIsForBrowser);
}

bool
ContentBridgeChild::DeallocPBrowserChild(PBrowserChild* aChild)
{
  return nsIContentChild::DeallocPBrowserChild(aChild);
}

mozilla::ipc::IPCResult
ContentBridgeChild::RecvPBrowserConstructor(PBrowserChild* aActor,
                                            const TabId& aTabId,
                                            const IPCTabContext& aContext,
                                            const uint32_t& aChromeFlags,
                                            const ContentParentId& aCpID,
                                            const bool& aIsForBrowser)
{
  return nsIContentChild::RecvPBrowserConstructor(aActor,
                                                  aTabId,
                                                  aContext,
                                                  aChromeFlags,
                                                  aCpID,
                                                  aIsForBrowser);
}

PBlobChild*
ContentBridgeChild::AllocPBlobChild(const BlobConstructorParams& aParams)
{
  return nsIContentChild::AllocPBlobChild(aParams);
}

bool
ContentBridgeChild::DeallocPBlobChild(PBlobChild* aActor)
{
  return nsIContentChild::DeallocPBlobChild(aActor);
}

PMemoryStreamChild*
ContentBridgeChild::AllocPMemoryStreamChild(const uint64_t& aSize)
{
  return nsIContentChild::AllocPMemoryStreamChild(aSize);
}

bool
ContentBridgeChild::DeallocPMemoryStreamChild(PMemoryStreamChild* aActor)
{
  return nsIContentChild::DeallocPMemoryStreamChild(aActor);
}

PChildToParentStreamChild*
ContentBridgeChild::AllocPChildToParentStreamChild()
{
  return nsIContentChild::AllocPChildToParentStreamChild();
}

bool
ContentBridgeChild::DeallocPChildToParentStreamChild(PChildToParentStreamChild* aActor)
{
  return nsIContentChild::DeallocPChildToParentStreamChild(aActor);
}

PParentToChildStreamChild*
ContentBridgeChild::AllocPParentToChildStreamChild()
{
  return nsIContentChild::AllocPParentToChildStreamChild();
}

bool
ContentBridgeChild::DeallocPParentToChildStreamChild(PParentToChildStreamChild* aActor)
{
  return nsIContentChild::DeallocPParentToChildStreamChild(aActor);
}

PFileDescriptorSetChild*
ContentBridgeChild::AllocPFileDescriptorSetChild(const FileDescriptor& aFD)
{
  return nsIContentChild::AllocPFileDescriptorSetChild(aFD);
}

bool
ContentBridgeChild::DeallocPFileDescriptorSetChild(PFileDescriptorSetChild* aActor)
{
  return nsIContentChild::DeallocPFileDescriptorSetChild(aActor);
}

mozilla::ipc::IPCResult
ContentBridgeChild::RecvActivate(PBrowserChild* aTab)
{
  TabChild* tab = static_cast<TabChild*>(aTab);
  return tab->RecvActivate();
}

mozilla::ipc::IPCResult
ContentBridgeChild::RecvDeactivate(PBrowserChild* aTab)
{
  TabChild* tab = static_cast<TabChild*>(aTab);
  return tab->RecvDeactivate();
}

mozilla::ipc::IPCResult
ContentBridgeChild::RecvParentActivated(PBrowserChild* aTab, const bool& aActivated)
{
  TabChild* tab = static_cast<TabChild*>(aTab);
  return tab->RecvParentActivated(aActivated);
}

} // namespace dom
} // namespace mozilla
