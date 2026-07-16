#if WITH_DEV_AUTOMATION_TESTS

#include "ObjectPoolTestNetworkActor.h"

#include "ObjectPoolSubsystem.h"

AObjectPoolTestNetworkActor::AObjectPoolTestNetworkActor()
{
	bReplicates = true;
	bReplicateUsingRegisteredSubObjectList = true;
	PrimaryActorTick.bCanEverTick = true;
	NetworkState = CreateDefaultSubobject<UPooledActorNetworkStateComponent>(TEXT("PoolNetworkState"));
}

void AObjectPoolTestNetworkActor::OnPoolCreated_Implementation()
{
	if (bDestroyNextOnCreated)
	{
		bDestroyNextOnCreated = false;
		Destroy();
	}
}

void AObjectPoolTestNetworkActor::OnPoolAcquireServer_Implementation(const FActorPoolAcquireContext& Context)
{
	if (bDestroyOnAcquire)
	{
		Destroy();
	}
}

void AObjectPoolTestNetworkActor::OnPoolAcquireClient_Implementation(const FActorPoolAcquireContext& Context)
{
	++ClientAcquireCount;
	LastClientAcquireContext = Context;
}

void AObjectPoolTestNetworkActor::OnPoolReleaseServer_Implementation()
{
	if (bAttemptReentrantRelease && PoolForReentrantRelease.IsValid())
	{
		StateObservedDuringRelease = PoolForReentrantRelease->GetActorPoolState(this);
		bReentrantReleaseResult = PoolForReentrantRelease->ReleaseActorToPool(this);
		UClass* AcquireClass = ReentrantAcquireClass != nullptr ? ReentrantAcquireClass.Get() : GetClass();
		ReentrantAcquireResult = PoolForReentrantRelease->SpawnActorFromPool(AcquireClass, FTransform::Identity);
	}
	if (bDestroyOnRelease)
	{
		Destroy();
	}
}

void AObjectPoolTestNetworkActor::OnPoolReleaseClient_Implementation()
{
	++ClientReleaseCount;
}

#endif
