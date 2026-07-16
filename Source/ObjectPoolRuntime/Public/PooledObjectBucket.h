#pragma once

#include "CoreMinimal.h"
#include "ObjectPoolTypes.h"
#include "PooledObjectBucket.generated.h"

/**
 * Stores the active and inactive instances for one pooled class.
 *
 * This type deliberately does not destroy objects or invoke business callbacks.
 * The owning subsystem is responsible for lifecycle policy; the bucket only
 * maintains membership and state invariants.
 */
USTRUCT()
struct OBJECTPOOLRUNTIME_API FPooledObjectBucket
{
	GENERATED_BODY()

public:
	void Configure(int32 InMaxPoolSize);

	/** Registers a newly created or externally supplied object as active. */
	EObjectPoolOperationResult RegisterActive(UObject* Object);

	/** Moves an active object to the inactive stack. */
	EObjectPoolOperationResult Release(UObject* Object);

	/** Removes and returns the most recently released valid object. */
	UObject* Acquire();

	/** Removes an object from the bucket regardless of its current state. */
	bool Remove(UObject* Object);

	/** Removes stale references and returns the number of entries removed. */
	int32 RemoveInvalidObjects();

	/** Clears membership without destroying the referenced objects. */
	void Reset();

	bool ContainsActive(const UObject* Object) const;
	bool ContainsInactive(const UObject* Object) const;
	EObjectPoolEntryState GetState(const UObject* Object) const;

	int32 NumActive() const { return ActiveObjects.Num(); }
	int32 NumInactive() const { return InactiveObjects.Num(); }
	int32 NumTotal() const { return NumActive() + NumInactive(); }
	int32 GetMaxPoolSize() const { return MaxPoolSize; }

	/** Appends every active and inactive member. Intended for ownership validation and diagnostics. */
	void GetObjects(TArray<UObject*>& OutObjects) const;

	/** Returns false and optionally explains the first broken invariant. */
	bool Validate(FString* OutError = nullptr) const;

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> InactiveObjects;

	UPROPERTY(Transient)
	TSet<TObjectPtr<UObject>> ActiveObjects;

	UPROPERTY(Transient)
	int32 MaxPoolSize = 64;
};
