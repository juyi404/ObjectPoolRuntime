#include "PooledActorNetworkStateComponent.h"

#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"
#include "ObjectPoolSubsystem.h"
#include "PoolableActorCallbackDispatch.h"
#include "PoolableActorInterface.h"

UPooledActorNetworkStateComponent::UPooledActorNetworkStateComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UPooledActorNetworkStateComponent::SetPoolActiveFromServer(const bool bNewActive)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	if (PoolState.bPoolActive == bNewActive)
	{
		return;
	}

	PoolState.bPoolActive = bNewActive;
}

void UPooledActorNetworkStateComponent::SetPoolAcquiredFromServer(const FActorPoolAcquireContext& Context)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	PoolState.bPoolActive = true;
	++PoolState.PoolGeneration;
	PoolState.AcquireContext = Context;
}

void UPooledActorNetworkStateComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UPooledActorNetworkStateComponent, PoolState);
}

void UPooledActorNetworkStateComponent::OnRep_PoolState()
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->GetClass()->ImplementsInterface(UPoolableActorInterface::StaticClass()))
	{
		return;
	}

	if (PoolState.bPoolActive)
	{
		if (bClientLifecycleActive && LastAppliedPoolGeneration == PoolState.PoolGeneration)
		{
			return;
		}

		// A generation jump while the client still considers the actor active means
		// Release and Acquire were collapsed into one replicated state update.
		if (bClientLifecycleActive && LastAppliedPoolGeneration != PoolState.PoolGeneration)
		{
			ObjectPoolActorCallbacks::ReleaseClient(Owner);
		}

		Owner->SetActorHiddenInGame(false);
		Owner->SetActorEnableCollision(true);
		Owner->SetActorTickEnabled(true);
		Owner->SetActorTransform(PoolState.AcquireContext.Transform, false, nullptr, ETeleportType::TeleportPhysics);
		Owner->SetOwner(PoolState.AcquireContext.Owner);
		Owner->SetInstigator(PoolState.AcquireContext.Instigator);
		ObjectPoolActorCallbacks::AcquireClient(Owner, PoolState.AcquireContext);
		bClientLifecycleActive = true;
		LastAppliedPoolGeneration = PoolState.PoolGeneration;
	}
	else
	{
		ObjectPoolActorCallbacks::ReleaseClient(Owner);
		Owner->SetActorTickEnabled(false);
		Owner->SetActorEnableCollision(false);
		Owner->SetActorHiddenInGame(true);
		bClientLifecycleActive = false;
		LastAppliedPoolGeneration = PoolState.PoolGeneration;
	}
}
