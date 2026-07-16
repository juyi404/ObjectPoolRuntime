#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ObjectPoolTypes.h"
#include "PooledActorNetworkStateComponent.generated.h"

USTRUCT()
struct FReplicatedActorPoolLifecycleState
{
	GENERATED_BODY()

	UPROPERTY()
	bool bPoolActive = true;

	UPROPERTY()
	int32 PoolGeneration = 0;

	UPROPERTY()
	FActorPoolAcquireContext AcquireContext;
};

/**
 * Replicates the logical pool lifecycle without changing the Actor's NetGUID.
 * The server owns state transitions; clients translate OnRep into pool callbacks.
 */
UCLASS(NotBlueprintable, Transient, ClassGroup = "Object Pool")
class OBJECTPOOLRUNTIME_API UPooledActorNetworkStateComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UPooledActorNetworkStateComponent();

	void SetPoolActiveFromServer(bool bNewActive);
	void SetPoolAcquiredFromServer(const FActorPoolAcquireContext& Context);

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	bool IsPoolActive() const { return PoolState.bPoolActive; }

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	int32 GetPoolGeneration() const { return PoolState.PoolGeneration; }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_PoolState)
	FReplicatedActorPoolLifecycleState PoolState;

	/** Client-only state used to reconstruct a release when multiple server transitions coalesce. */
	bool bClientLifecycleActive = false;
	int32 LastAppliedPoolGeneration = INDEX_NONE;

	UFUNCTION()
	void OnRep_PoolState();
};
