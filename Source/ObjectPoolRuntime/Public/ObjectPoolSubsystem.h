#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ObjectPoolTypes.h"
#include "PooledObjectBucket.h"
#include "ObjectPoolSubsystem.generated.h"

struct FStreamableHandle;
class UActorComponent;

USTRUCT(BlueprintType)
struct OBJECTPOOLRUNTIME_API FObjectPoolClassStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Object Pool")
	int32 Active = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Object Pool")
	int32 Inactive = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Object Pool")
	int32 Created = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Object Pool")
	int32 Preallocated = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Object Pool")
	int32 PoolHits = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Object Pool")
	int32 PoolMisses = 0;
};

USTRUCT(BlueprintType)
struct OBJECTPOOLRUNTIME_API FObjectPoolClassSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Object Pool")
	TObjectPtr<UClass> ObjectClass = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Object Pool")
	FObjectPoolClassStats Stats;

	UPROPERTY(BlueprintReadOnly, Category = "Object Pool")
	int32 MaxPoolSize = 0;
};

USTRUCT()
struct OBJECTPOOLRUNTIME_API FObjectPoolReplicatedOwnerBinding
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<AActor> Actor = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UActorComponent> Component = nullptr;
};

/**
 * World-scoped owner of all actor, UObject, and component pools.
 * All mutation APIs (acquire, release, prewarm, and mode switching) are game-thread only.
 */
UCLASS()
class OBJECTPOOLRUNTIME_API UObjectPoolSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	/** Gets an inactive actor or creates one when the class configuration permits growth. */
	UFUNCTION(BlueprintCallable, Category = "Object Pool", meta = (DeterminesOutputType = "ActorClass"))
	AActor* SpawnActorFromPool(
		TSubclassOf<AActor> ActorClass,
		const FTransform& Transform,
		AActor* Owner = nullptr,
		APawn* Instigator = nullptr);

	/** Returns an active, subsystem-owned actor to its class bucket. */
	UFUNCTION(BlueprintCallable, Category = "Object Pool")
	bool ReleaseActorToPool(AActor* Actor);

	/** Gets an object and reparents it to the requested Outer. */
	UFUNCTION(BlueprintCallable, Category = "Object Pool", meta = (DeterminesOutputType = "ObjectClass"))
	UObject* GetObjectFromPool(UObject* Outer, TSubclassOf<UObject> ObjectClass);

	/** Returns an active, subsystem-owned UObject to the dedicated pool Outer. */
	UFUNCTION(BlueprintCallable, Category = "Object Pool")
	bool ReleaseObjectToPool(UObject* Object);

	/** Gets a dynamically created component and assigns it to the requested owner. */
	UFUNCTION(BlueprintCallable, Category = "Object Pool", meta = (DeterminesOutputType = "ComponentClass"))
	UActorComponent* GetComponentFromPool(AActor* Owner, TSubclassOf<UActorComponent> ComponentClass);

	UFUNCTION(BlueprintCallable, Category = "Object Pool")
	bool ReleaseComponentToPool(UActorComponent* Component);

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	EObjectPoolEntryState GetActorPoolState(const AActor* Actor) const;

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	EObjectPoolEntryState GetObjectPoolState(const UObject* Object) const;

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	EObjectPoolEntryState GetComponentPoolState(const UActorComponent* Component) const;

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	FGameplayTag GetCurrentModeTag() const { return CurrentModeTag; }

	/** Switches the active configuration when no pooled entries are active, then rebuilds and prewarms the pools. */
	UFUNCTION(BlueprintCallable, Category = "Object Pool")
	bool SwitchModeTag(FGameplayTag NewModeTag);

	/** Re-evaluates the configured GameState provider and switches to the returned mode when safe. */
	UFUNCTION(BlueprintCallable, Category = "Object Pool")
	bool RefreshModeTag();

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	FObjectPoolClassStats GetActorPoolStats(TSubclassOf<AActor> ActorClass) const;

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	FObjectPoolClassStats GetObjectPoolStats(TSubclassOf<UObject> ObjectClass) const;

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	FObjectPoolClassStats GetComponentPoolStats(TSubclassOf<UActorComponent> ComponentClass) const;

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	bool IsActorPrewarmComplete() const { return bActorPrewarmComplete; }

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	bool IsObjectPrewarmComplete() const { return bObjectPrewarmComplete; }

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	bool IsComponentPrewarmComplete() const { return bComponentPrewarmComplete; }

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	TArray<FObjectPoolClassSnapshot> GetActorPoolSnapshots() const;

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	TArray<FObjectPoolClassSnapshot> GetObjectPoolSnapshots() const;

	UFUNCTION(BlueprintPure, Category = "Object Pool")
	TArray<FObjectPoolClassSnapshot> GetComponentPoolSnapshots() const;

	/** Human-readable report used by Pool.Dump and diagnostics. */
	FString BuildDebugReport() const;

	/** Checks all buckets and ownership records. Intended for tests and debug commands. */
	bool Validate(FString* OutError = nullptr) const;

private:
	const FActorPoolClassConfig* FindActorConfig(UClass* ActorClass) const;
	const FObjectPoolClassConfig* FindObjectConfig(UClass* ObjectClass) const;
	const FComponentPoolClassConfig* FindComponentConfig(UClass* ComponentClass) const;
	const FPoolModeConfig* FindCurrentModeConfig() const;
	FGameplayTag ResolveCurrentModeTag() const;
	FPooledObjectBucket& FindOrAddActorBucket(UClass* ActorClass, const FActorPoolClassConfig& Config);
	AActor* CreateActor(UClass* ActorClass, const FTransform& Transform, AActor* Owner, APawn* Instigator);
	void ActivateActor(AActor* Actor, const FActorPoolAcquireContext& Context);
	void DeactivateActor(AActor* Actor, EObjectPoolRecoveryPolicy RecoveryPolicy);
	void InvokeAcquireCallback(AActor* Actor, const FActorPoolAcquireContext& Context) const;
	void InvokeReleaseCallback(AActor* Actor) const;
	void RemoveInvalidManagedActors();
	void BeginActorPrewarm();
	void CompleteActorPrewarm();
	void PreallocateActorClass(const FActorPoolClassConfig& Config);
	int32 GetPreallocateCount(const FPoolClassConfigBase& Config) const;
	void ValidateAfterMutation(const TCHAR* Operation) const;
	FPooledObjectBucket& FindOrAddObjectBucket(UClass* ObjectClass, const FObjectPoolClassConfig& Config);
	UObject* CreateObject(UClass* ObjectClass);
	bool MoveObjectToOuter(UObject* Object, UObject* NewOuter) const;
	void InvokeObjectCreatedCallback(UObject* Object) const;
	void InvokeObjectAcquireCallback(UObject* Object) const;
	void InvokeObjectReleaseCallback(UObject* Object) const;
	void DeactivateObject(UObject* Object, EObjectPoolRecoveryPolicy RecoveryPolicy) const;
	void RemoveInvalidManagedObjects();
	void BeginObjectPrewarm();
	void CompleteObjectPrewarm();
	void PreallocateObjectClass(const FObjectPoolClassConfig& Config);
	AActor* EnsureComponentPoolOwner();
	FPooledObjectBucket& FindOrAddComponentBucket(UClass* ComponentClass, const FComponentPoolClassConfig& Config);
	UActorComponent* CreateComponent(UClass* ComponentClass);
	bool MoveComponentToOwner(UActorComponent* Component, AActor* NewOwner);
	void InvokeComponentCreatedCallback(UActorComponent* Component) const;
	void InvokeComponentAcquireCallback(UActorComponent* Component) const;
	void InvokeComponentReleaseCallback(UActorComponent* Component) const;
	void DeactivateComponent(UActorComponent* Component, EObjectPoolRecoveryPolicy RecoveryPolicy);
	void RemoveInvalidManagedComponents();
	void BeginComponentPrewarm();
	void CompleteComponentPrewarm();
	void PreallocateComponentClass(const FComponentPoolClassConfig& Config);
	void RegisterObjectForReplication(UObject* Object, UObject* RequestedOuter, const FObjectPoolClassConfig& Config);
	void UnregisterObjectFromReplication(UObject* Object, EObjectPoolRemoteSubObjectReleasePolicy ReleasePolicy);
	void ActivateComponentReplication(UActorComponent* Component, const FComponentPoolClassConfig& Config);
	void DeactivateComponentReplication(UActorComponent* Component, EObjectPoolRemoteSubObjectReleasePolicy ReleasePolicy);
	class UPooledActorNetworkStateComponent* EnsureActorNetworkStateComponent(AActor* Actor);
	void PrepareActorNetworkAcquire(AActor* Actor, const FActorPoolAcquireContext& Context);
	void PrepareActorNetworkRelease(AActor* Actor);
	bool HasAnyActiveEntries() const;
	void ResetPoolsForModeSwitch();

	UPROPERTY(Transient)
	TMap<TObjectPtr<UClass>, FPooledObjectBucket> ActorPools;

	UPROPERTY(Transient)
	TMap<TObjectPtr<AActor>, TObjectPtr<UClass>> ManagedActors;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UClass>, FObjectPoolClassStats> ActorStats;

	UPROPERTY(Transient)
	TMap<TObjectPtr<AActor>, EObjectPoolEntryState> ActorTransitions;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UClass>, FPooledObjectBucket> ObjectPools;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UObject>, TObjectPtr<UClass>> ManagedObjects;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UClass>, FObjectPoolClassStats> ObjectStats;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UObject>, EObjectPoolEntryState> ObjectTransitions;

	UPROPERTY(Transient)
	TObjectPtr<UObject> PoolOuter = nullptr;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UClass>, FPooledObjectBucket> ComponentPools;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UActorComponent>, TObjectPtr<UClass>> ManagedComponents;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UClass>, FObjectPoolClassStats> ComponentStats;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UActorComponent>, EObjectPoolEntryState> ComponentTransitions;

	UPROPERTY(Transient)
	TObjectPtr<AActor> ComponentPoolOwner = nullptr;

	UPROPERTY(Transient)
	TMap<TObjectPtr<UObject>, FObjectPoolReplicatedOwnerBinding> ReplicatedObjectOwners;

	UPROPERTY(Transient)
	FGameplayTag CurrentModeTag;

	bool bIsDeinitializing = false;
	bool bActorPrewarmComplete = false;
	bool bObjectPrewarmComplete = false;
	bool bComponentPrewarmComplete = false;

	TSharedPtr<FStreamableHandle> ActorPrewarmHandle;
	TSharedPtr<FStreamableHandle> ObjectPrewarmHandle;
	TSharedPtr<FStreamableHandle> ComponentPrewarmHandle;
};
