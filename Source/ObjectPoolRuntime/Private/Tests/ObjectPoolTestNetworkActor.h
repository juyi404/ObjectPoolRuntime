#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PoolableActorInterface.h"
#include "PooledActorNetworkStateComponent.h"
#include "ObjectPoolTestNetworkActor.generated.h"

class UObjectPoolSubsystem;

UCLASS()
class AObjectPoolTestNetworkActor : public AActor, public IPoolableActorInterface
{
	GENERATED_BODY()

public:
	AObjectPoolTestNetworkActor();

	virtual void OnPoolAcquireClient_Implementation(const FActorPoolAcquireContext& Context) override;
	virtual void OnPoolReleaseServer_Implementation() override;

	virtual void OnPoolReleaseClient_Implementation() override;

	UPooledActorNetworkStateComponent* GetNetworkState() const { return NetworkState; }

	UFUNCTION()
	void TestTimerCallback() {}

	UPROPERTY(Transient)
	int32 ClientAcquireCount = 0;

	UPROPERTY(Transient)
	int32 ClientReleaseCount = 0;

	UPROPERTY(Transient)
	FActorPoolAcquireContext LastClientAcquireContext;

	bool bAttemptReentrantRelease = false;
	bool bReentrantReleaseResult = false;
	bool bDestroyOnRelease = false;
	EObjectPoolEntryState StateObservedDuringRelease = EObjectPoolEntryState::Unmanaged;
	TObjectPtr<AActor> ReentrantAcquireResult = nullptr;
	TWeakObjectPtr<UObjectPoolSubsystem> PoolForReentrantRelease;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPooledActorNetworkStateComponent> NetworkState;
};

/** Verifies that a poolable interface and its callbacks are inherited without any gameplay module. */
UCLASS()
class AObjectPoolTestDerivedNetworkActor : public AObjectPoolTestNetworkActor
{
	GENERATED_BODY()
};
