#pragma once

#include "GameFramework/Actor.h"
#include "PoolableActorInterface.h"

namespace ObjectPoolActorCallbacks
{
	inline bool UsesBlueprintDispatch(const AActor* Actor)
	{
		return Actor != nullptr && Actor->GetClass()->HasAnyClassFlags(CLASS_CompiledFromBlueprint);
	}

	inline IPoolableActorInterface* GetNativeInterface(AActor* Actor)
	{
		return Actor != nullptr ? Cast<IPoolableActorInterface>(Actor) : nullptr;
	}

	inline void Created(AActor* Actor)
	{
		if (UsesBlueprintDispatch(Actor))
		{
			IPoolableActorInterface::Execute_OnPoolCreated(Actor);
		}
		else if (IPoolableActorInterface* Interface = GetNativeInterface(Actor))
		{
			Interface->OnPoolCreated_Implementation();
		}
	}

	inline void AcquireServer(AActor* Actor, const FActorPoolAcquireContext& Context)
	{
		if (UsesBlueprintDispatch(Actor))
		{
			IPoolableActorInterface::Execute_OnPoolAcquireServer(Actor, Context);
		}
		else if (IPoolableActorInterface* Interface = GetNativeInterface(Actor))
		{
			Interface->OnPoolAcquireServer_Implementation(Context);
		}
	}

	inline void AcquireClient(AActor* Actor, const FActorPoolAcquireContext& Context)
	{
		if (UsesBlueprintDispatch(Actor))
		{
			IPoolableActorInterface::Execute_OnPoolAcquireClient(Actor, Context);
		}
		else if (IPoolableActorInterface* Interface = GetNativeInterface(Actor))
		{
			Interface->OnPoolAcquireClient_Implementation(Context);
		}
	}

	inline void ReleaseServer(AActor* Actor)
	{
		if (UsesBlueprintDispatch(Actor))
		{
			IPoolableActorInterface::Execute_OnPoolReleaseServer(Actor);
		}
		else if (IPoolableActorInterface* Interface = GetNativeInterface(Actor))
		{
			Interface->OnPoolReleaseServer_Implementation();
		}
	}

	inline void ReleaseClient(AActor* Actor)
	{
		if (UsesBlueprintDispatch(Actor))
		{
			IPoolableActorInterface::Execute_OnPoolReleaseClient(Actor);
		}
		else if (IPoolableActorInterface* Interface = GetNativeInterface(Actor))
		{
			Interface->OnPoolReleaseClient_Implementation();
		}
	}
}
