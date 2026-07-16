#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "PoolableActorInterface.h"
#include "PoolableComponentInterface.h"
#include "PoolableObjectInterface.h"
#include "ObjectPoolNetworkSmokeActor.generated.h"

class UPooledActorNetworkStateComponent;

UCLASS(NotBlueprintable, Transient)
class UObjectPoolNetworkSmokeObject final : public UObject, public IPoolableObjectInterface
{
	GENERATED_BODY()

public:
	virtual bool IsSupportedForNetworking() const override { return true; }
	virtual void OnObjectAcquired_Implementation() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_AcquireSerial)
	int32 AcquireSerial = 0;

	UPROPERTY(Replicated)
	FString ServerOwnerName;

	UFUNCTION()
	void OnRep_AcquireSerial();
};

UCLASS(NotBlueprintable, Transient)
class UObjectPoolNetworkSmokeComponent final : public UActorComponent, public IPoolableComponentInterface
{
	GENERATED_BODY()

public:
	UObjectPoolNetworkSmokeComponent();
	virtual bool GetComponentClassCanReplicate() const override { return true; }
	virtual void OnComponentAcquired_Implementation() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY(ReplicatedUsing = OnRep_AcquireSerial)
	int32 AcquireSerial = 0;

	UPROPERTY(Replicated)
	FString ServerOwnerName;

	UFUNCTION()
	void OnRep_AcquireSerial();
};

/** Development-only replicated actor used by Pool.NetSmoke. */
UCLASS(NotBlueprintable, Transient)
class AObjectPoolNetworkSmokeActor final : public AActor, public IPoolableActorInterface
{
	GENERATED_BODY()

public:
	AObjectPoolNetworkSmokeActor();

	virtual void OnPoolAcquireServer_Implementation(const FActorPoolAcquireContext& Context) override;
	virtual void OnPoolAcquireClient_Implementation(const FActorPoolAcquireContext& Context) override;
	virtual void OnPoolReleaseServer_Implementation() override;
	virtual void OnPoolReleaseClient_Implementation() override;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UPooledActorNetworkStateComponent> NetworkState;
};
