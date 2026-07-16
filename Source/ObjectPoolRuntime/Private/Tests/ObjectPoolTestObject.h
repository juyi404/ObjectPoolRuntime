#pragma once

#include "CoreMinimal.h"
#include "ObjectPoolSubsystem.h"
#include "PoolableObjectInterface.h"
#include "ObjectPoolTestObject.generated.h"

UCLASS()
class UObjectPoolTestObject : public UObject, public IPoolableObjectInterface
{
	GENERATED_BODY()

public:
	virtual bool IsSupportedForNetworking() const override { return true; }
	virtual void OnObjectPoolCreated_Implementation() override
	{
		++CreatedCount;
		if (bMarkNextCreatedGarbage)
		{
			bMarkNextCreatedGarbage = false;
			MarkAsGarbage();
		}
	}
	virtual void OnObjectAcquired_Implementation() override
	{
		++AcquiredCount;
		if (bMarkGarbageOnAcquire)
		{
			MarkAsGarbage();
		}
	}
	virtual void OnObjectReleased_Implementation() override
	{
		++ReleasedCount;
		if (bAttemptReentrantRelease && PoolForReentrantRelease.IsValid())
		{
			StateObservedDuringRelease = PoolForReentrantRelease->GetObjectPoolState(this);
			bReentrantReleaseResult = PoolForReentrantRelease->ReleaseObjectToPool(this);
			ReentrantAcquireResult = PoolForReentrantRelease->GetObjectFromPool(GetOuter(), GetClass());
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
	TObjectPtr<UObject> ReentrantAcquireResult = nullptr;
	EObjectPoolEntryState StateObservedDuringRelease = EObjectPoolEntryState::Unmanaged;
	TWeakObjectPtr<UObjectPoolSubsystem> PoolForReentrantRelease;
};
