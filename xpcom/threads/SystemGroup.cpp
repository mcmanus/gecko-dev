/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SystemGroup.h"

#include "mozilla/Move.h"
#include "mozilla/UniquePtr.h"
#include "nsINamed.h"

using namespace mozilla;

class SystemGroupImpl final : public ValidatingDispatcher
{
public:
  SystemGroupImpl();
  ~SystemGroupImpl() {}

  static void InitStatic();
  static void ShutdownStatic();
  static SystemGroupImpl* Get();

  NS_METHOD_(MozExternalRefCountType) AddRef(void)
  {
    return 2;
  }
  NS_METHOD_(MozExternalRefCountType) Release(void)
  {
    return 1;
  }

private:
  static UniquePtr<SystemGroupImpl> sSingleton;
};

UniquePtr<SystemGroupImpl> SystemGroupImpl::sSingleton;

SystemGroupImpl::SystemGroupImpl()
{
  CreateEventTargets(/* aNeedValidation = */ true);
}

/* static */ void
SystemGroupImpl::InitStatic()
{
  MOZ_ASSERT(!sSingleton);
  MOZ_ASSERT(NS_IsMainThread());
  sSingleton = MakeUnique<SystemGroupImpl>();
}

/* static */ void
SystemGroupImpl::ShutdownStatic()
{
  sSingleton->Shutdown(true);
  sSingleton = nullptr;
}

/* static */ SystemGroupImpl*
SystemGroupImpl::Get()
{
  MOZ_ASSERT(sSingleton);
  return sSingleton.get();
}

void
SystemGroup::InitStatic()
{
  SystemGroupImpl::InitStatic();
}

void
SystemGroup::Shutdown()
{
  SystemGroupImpl::ShutdownStatic();
}

/* static */ nsresult
SystemGroup::Dispatch(const char* aName,
                      TaskCategory aCategory,
                      already_AddRefed<nsIRunnable>&& aRunnable)
{
  return SystemGroupImpl::Get()->Dispatch(aName, aCategory, Move(aRunnable));
}

/* static */ nsIEventTarget*
SystemGroup::EventTargetFor(TaskCategory aCategory)
{
  return SystemGroupImpl::Get()->EventTargetFor(aCategory);
}

/* static */ AbstractThread*
SystemGroup::AbstractMainThreadFor(TaskCategory aCategory)
{
  return SystemGroupImpl::Get()->AbstractMainThreadFor(aCategory);
}
