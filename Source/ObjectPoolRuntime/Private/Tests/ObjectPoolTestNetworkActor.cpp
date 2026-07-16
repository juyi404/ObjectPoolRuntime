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
		ReentrantAcquireResult = PoolForReentrantRelease->SpawnActorFromPool(GetClass(), FTransform::Identity);
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
