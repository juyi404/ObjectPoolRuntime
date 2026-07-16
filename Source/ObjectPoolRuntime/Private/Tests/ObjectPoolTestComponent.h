#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ObjectPoolSubsystem.h"
#include "PoolableComponentInterface.h"
#include "ObjectPoolTestComponent.generated.h"

UCLASS()
class UObjectPoolTestComponent : public UActorComponent, public IPoolableComponentInterface
{
	GENERATED_BODY()

public:
	virtual bool GetComponentClassCanReplicate() const override { return true; }
	virtual void OnComponentPoolCreated_Implementation() override
	{
		++CreatedCount;
		if (bMarkNextCreatedGarbage)
		{
			bMarkNextCreatedGarbage = false;
			MarkAsGarbage();
		}
	}
	virtual void OnComponentAcquired_Implementation() override
	{
		++AcquiredCount;
		if (bMarkGarbageOnAcquire)
		{
			MarkAsGarbage();
		}
	}
	virtual void OnComponentReleased_Implementation() override
	{
		++ReleasedCount;
		if (bAttemptReentrantRelease && PoolForReentrantRelease.IsValid())
		{
			StateObservedDuringRelease = PoolForReentrantRelease->GetComponentPoolState(this);
			bReentrantReleaseResult = PoolForReentrantRelease->ReleaseComponentToPool(this);
			ReentrantAcquireResult = PoolForReentrantRelease->GetComponentFromPool(GetOwner(), GetClass());
		}
		if (bMarkGarbageOnRelease)
		{
			MarkAsGarbage();
		}
	}

	UFUNCTION()
	void TestTimerCallback() {}

	int32 CreatedCount = 0;
	int32 AcquiredCount = 0;
	int32 ReleasedCount = 0;
	bool bAttemptReentrantRelease = false;
	bool bReentrantReleaseResult = false;
	bool bMarkGarbageOnRelease = false;
	bool bMarkGarbageOnAcquire = false;
	inline static bool bMarkNextCreatedGarbage = false;
	EObjectPoolEntryState StateObservedDuringRelease = EObjectPoolEntryState::Unmanaged;
	TObjectPtr<UActorComponent> ReentrantAcquireResult = nullptr;
	TWeakObjectPtr<UObjectPoolSubsystem> PoolForReentrantRelease;
};
