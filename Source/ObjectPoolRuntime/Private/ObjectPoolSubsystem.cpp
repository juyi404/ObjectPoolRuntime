#include "ObjectPoolSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/AssetManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/GameStateBase.h"
#include "ObjectPoolRuntime.h"
#include "ObjectPoolContainer.h"
#include "ObjectPoolSettings.h"
#include "PoolableActorInterface.h"
#include "PoolableActorCallbackDispatch.h"
#include "PoolableObjectInterface.h"
#include "PoolableComponentInterface.h"
#include "PooledActorNetworkStateComponent.h"
#include "TimerManager.h"
#include "Misc/ScopeExit.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

bool UObjectPoolSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UObjectPoolSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bIsDeinitializing = false;
	bActorPrewarmComplete = false;
	bObjectPrewarmComplete = false;
	bComponentPrewarmComplete = false;
	PoolOuter = NewObject<UObjectPoolContainer>(this, TEXT("ObjectPoolOuter"), RF_Transient);
	CurrentModeTag = ResolveCurrentModeTag();
	RefreshSettingsValidity();
}

void UObjectPoolSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	check(IsInGameThread());
	Super::OnWorldBeginPlay(InWorld);
	if (!RefreshSettingsValidity())
	{
		bActorPrewarmComplete = true;
		bObjectPrewarmComplete = true;
		bComponentPrewarmComplete = true;
		return;
	}
	CurrentModeTag = ResolveCurrentModeTag();
	BeginActorPrewarm();
	BeginObjectPrewarm();
	BeginComponentPrewarm();
}

bool UObjectPoolSubsystem::SwitchModeTag(const FGameplayTag NewModeTag)
{
	check(IsInGameThread());
	if (bIsDeinitializing)
	{
		return false;
	}
	if (!RefreshSettingsValidity())
	{
		return false;
	}
	if (CurrentModeTag == NewModeTag)
	{
		return true;
	}
	if (HasAnyActiveEntries() || !ActorTransitions.IsEmpty() || !ObjectTransitions.IsEmpty() || !ComponentTransitions.IsEmpty())
	{
		UE_LOG(LogObjectPool, Log,
			TEXT("Cannot switch object-pool mode from %s to %s while entries are active or transitioning."),
			*CurrentModeTag.ToString(), *NewModeTag.ToString());
		return false;
	}

	ResetPoolsForModeSwitch();
	CurrentModeTag = NewModeTag;
	BeginActorPrewarm();
	BeginObjectPrewarm();
	BeginComponentPrewarm();
	return true;
}

bool UObjectPoolSubsystem::RefreshModeTag()
{
	check(IsInGameThread());
	return SwitchModeTag(ResolveCurrentModeTag());
}

void UObjectPoolSubsystem::Deinitialize()
{
	bIsDeinitializing = true;
	if (ActorPrewarmHandle.IsValid())
	{
		ActorPrewarmHandle->CancelHandle();
		ActorPrewarmHandle.Reset();
	}
	if (ObjectPrewarmHandle.IsValid())
	{
		ObjectPrewarmHandle->CancelHandle();
		ObjectPrewarmHandle.Reset();
	}
	if (ComponentPrewarmHandle.IsValid())
	{
		ComponentPrewarmHandle->CancelHandle();
		ComponentPrewarmHandle.Reset();
	}

	for (const TPair<TObjectPtr<AActor>, TObjectPtr<UClass>>& Pair : ManagedActors)
	{
		if (IsValid(Pair.Key))
		{
			Pair.Key->Destroy();
		}
	}

	ManagedActors.Reset();
	ActorPools.Reset();
	ActorStats.Reset();
	ActorTransitions.Reset();
	ManagedObjects.Reset();
	ObjectPools.Reset();
	ObjectStats.Reset();
	ObjectTransitions.Reset();
	ReplicatedObjectOwners.Reset();
	PoolOuter = nullptr;
	ManagedComponents.Reset();
	ComponentPools.Reset();
	ComponentStats.Reset();
	ComponentTransitions.Reset();
	if (IsValid(ComponentPoolOwner))
	{
		ComponentPoolOwner->Destroy();
	}
	ComponentPoolOwner = nullptr;
	CurrentModeTag = FGameplayTag();
	bActorPrewarmComplete = false;
	bObjectPrewarmComplete = false;
	bComponentPrewarmComplete = false;
	Super::Deinitialize();
}

AActor* UObjectPoolSubsystem::SpawnActorFromPool(
	const TSubclassOf<AActor> ActorClass,
	const FTransform& Transform,
	AActor* Owner,
	APawn* Instigator)
{
	check(IsInGameThread());
	UWorld* World = GetWorld();
	UClass* Class = ActorClass.Get();
	const UObjectPoolSettings* Settings = GetDefault<UObjectPoolSettings>();
	if (bIsDeinitializing || !bSettingsValid || World == nullptr || Class == nullptr || !Settings->bEnabled || !IsObjectPoolRuntimeEnabled())
	{
		if (IsObjectPoolDebugLoggingEnabled())
		{
			UE_LOG(LogObjectPool, Verbose, TEXT("Actor pool acquisition disabled or unavailable for %s."), *GetNameSafe(Class));
		}
		return nullptr;
	}
	if (!Class->IsChildOf(AActor::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Cannot pool invalid actor class %s."), *GetNameSafe(Class));
		return nullptr;
	}
	if (IsActorClassTransitioning(Class))
	{
		return nullptr;
	}

	const FActorPoolClassConfig* Config = FindActorConfig(Class);
	if (Config == nullptr || !Config->bEnabled)
	{
		if (IsObjectPoolDebugLoggingEnabled())
		{
			UE_LOG(LogObjectPool, Verbose, TEXT("Actor class %s is not enabled in the current pool mode."), *GetNameSafe(Class));
		}
		return nullptr;
	}

	FPooledObjectBucket& Bucket = FindOrAddActorBucket(Class, *Config);
	RemoveInvalidManagedActors();
	Bucket.RemoveInvalidObjects();
	FObjectPoolClassStats& Stats = ActorStats.FindOrAdd(Class);

	AActor* Actor = Cast<AActor>(Bucket.Acquire());
	if (Actor != nullptr)
	{
		++Stats.PoolHits;
		if (IsObjectPoolDebugLoggingEnabled())
		{
			UE_LOG(LogObjectPool, Verbose, TEXT("Pool hit: %s -> %s."), *GetNameSafe(Class), *GetNameSafe(Actor));
		}
	}
	else
	{
		++Stats.PoolMisses;
		if (!Config->bAllowRuntimeGrowth || Bucket.NumTotal() >= Config->MaxPoolSize)
		{
			if (IsObjectPoolDebugLoggingEnabled())
			{
				UE_LOG(LogObjectPool, Verbose, TEXT("Pool miss without growth: %s (%d/%d)."),
					*GetNameSafe(Class), Bucket.NumTotal(), Config->MaxPoolSize);
			}
			return nullptr;
		}

		Actor = CreateActor(Class, Transform, Owner, Instigator);
		if (Actor == nullptr)
		{
			return nullptr;
		}

		const EObjectPoolOperationResult RegisterResult = Bucket.RegisterActive(Actor);
		if (RegisterResult != EObjectPoolOperationResult::Succeeded)
		{
			Actor->Destroy();
			return nullptr;
		}

		ManagedActors.Add(Actor, Class);
		++Stats.Created;
		if (Actor->GetClass()->ImplementsInterface(UPoolableActorInterface::StaticClass()))
		{
			ObjectPoolActorCallbacks::Created(Actor);
		}
		if (!IsValid(Actor) || ManagedActors.Find(Actor) == nullptr || !Bucket.ContainsActive(Actor))
		{
			Bucket.Remove(Actor);
			ManagedActors.Remove(Actor);
			return nullptr;
		}
	}

	FActorPoolAcquireContext Context;
	Context.Transform = Transform;
	Context.Owner = Owner;
	Context.Instigator = Instigator;
	Context.ModeTag = CurrentModeTag;
	if (!ActivateActor(Actor, Context))
	{
		Bucket.Remove(Actor);
		ManagedActors.Remove(Actor);
		return nullptr;
	}
	ValidateAfterMutation(TEXT("AcquireActor"));
	return Actor;
}

bool UObjectPoolSubsystem::ReleaseActorToPool(AActor* Actor)
{
	check(IsInGameThread());
	if (bIsDeinitializing || !IsValid(Actor) || Actor->GetWorld() != GetWorld())
	{
		return false;
	}

	TObjectPtr<UClass>* ManagedClass = ManagedActors.Find(Actor);
	if (ManagedClass == nullptr)
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Rejected unmanaged actor release: %s."), *GetNameSafe(Actor));
		return false;
	}

	FPooledObjectBucket* Bucket = ActorPools.Find(*ManagedClass);
	const FActorPoolClassConfig* Config = FindActorConfig(*ManagedClass);
	if (Bucket == nullptr || Config == nullptr || !Bucket->ContainsActive(Actor))
	{
		return false;
	}
	if (ActorTransitions.Contains(Actor))
	{
		return false;
	}

	ActorTransitions.Add(Actor, EObjectPoolEntryState::Releasing);
	ON_SCOPE_EXIT
	{
		ActorTransitions.Remove(Actor);
	};

	InvokeReleaseCallback(Actor);
	if (!IsValid(Actor) || ManagedActors.Find(Actor) == nullptr || !Bucket->ContainsActive(Actor))
	{
		Bucket->Remove(Actor);
		ManagedActors.Remove(Actor);
		return false;
	}
	DeactivateActor(Actor, Config->RecoveryPolicy);

	const EObjectPoolOperationResult Result = Bucket->Release(Actor);
	if (Result == EObjectPoolOperationResult::Succeeded)
	{
		if (IsObjectPoolDebugLoggingEnabled())
		{
			UE_LOG(LogObjectPool, Verbose, TEXT("Released actor %s to pool %s."), *GetNameSafe(Actor), *GetNameSafe(*ManagedClass));
		}
		ValidateAfterMutation(TEXT("ReleaseActor"));
		return true;
	}

	if (Result == EObjectPoolOperationResult::CapacityReached)
	{
		Bucket->Remove(Actor);
		ManagedActors.Remove(Actor);
		Actor->Destroy();
	}

	return false;
}

UObject* UObjectPoolSubsystem::GetObjectFromPool(UObject* Outer, const TSubclassOf<UObject> ObjectClass)
{
	check(IsInGameThread());
	UClass* Class = ObjectClass.Get();
	const UObjectPoolSettings* Settings = GetDefault<UObjectPoolSettings>();
	if (bIsDeinitializing || !bSettingsValid || Outer == nullptr || Class == nullptr || PoolOuter == nullptr ||
		!Settings->bEnabled || !IsObjectPoolRuntimeEnabled())
	{
		return nullptr;
	}
	if (Class->IsChildOf(AActor::StaticClass()) || Class->IsChildOf(UActorComponent::StaticClass()) ||
		Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Cannot use class %s in the UObject pool."), *GetNameSafe(Class));
		return nullptr;
	}
	if (IsObjectClassTransitioning(Class))
	{
		return nullptr;
	}

	if (UWorld* OuterWorld = Outer->GetWorld(); OuterWorld != nullptr && OuterWorld != GetWorld())
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Rejected cross-world UObject pool request for %s."), *GetNameSafe(Class));
		return nullptr;
	}

	const FObjectPoolClassConfig* Config = FindObjectConfig(Class);
	if (Config == nullptr || !Config->bEnabled)
	{
		return nullptr;
	}

	FPooledObjectBucket& Bucket = FindOrAddObjectBucket(Class, *Config);
	RemoveInvalidManagedObjects();
	Bucket.RemoveInvalidObjects();
	FObjectPoolClassStats& Stats = ObjectStats.FindOrAdd(Class);

	UObject* Object = Bucket.Acquire();
	if (Object != nullptr)
	{
		++Stats.PoolHits;
	}
	else
	{
		++Stats.PoolMisses;
		if (!Config->bAllowRuntimeGrowth || Bucket.NumTotal() >= Config->MaxPoolSize)
		{
			return nullptr;
		}

		Object = CreateObject(Class);
		if (Object == nullptr || Bucket.RegisterActive(Object) != EObjectPoolOperationResult::Succeeded)
		{
			return nullptr;
		}

		ManagedObjects.Add(Object, Class);
		++Stats.Created;
		InvokeObjectCreatedCallback(Object);
	}

	if (!MoveObjectToOuter(Object, Outer))
	{
		Bucket.Remove(Object);
		ManagedObjects.Remove(Object);
		Object->MarkAsGarbage();
		return nullptr;
	}

	RegisterObjectForReplication(Object, Outer, *Config);
	InvokeObjectAcquireCallback(Object);
	if (!IsValid(Object) || ManagedObjects.Find(Object) == nullptr || !Bucket.ContainsActive(Object))
	{
		UnregisterObjectFromReplication(Object, EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica);
		Bucket.Remove(Object);
		ManagedObjects.Remove(Object);
		ReplicatedObjectOwners.Remove(Object);
		return nullptr;
	}
	ValidateAfterMutation(TEXT("AcquireObject"));
	return Object;
}

bool UObjectPoolSubsystem::ReleaseObjectToPool(UObject* Object)
{
	check(IsInGameThread());
	if (bIsDeinitializing || !IsValid(Object) || PoolOuter == nullptr)
	{
		return false;
	}

	TObjectPtr<UClass>* ManagedClass = ManagedObjects.Find(Object);
	if (ManagedClass == nullptr)
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Rejected unmanaged UObject release: %s."), *GetNameSafe(Object));
		return false;
	}

	FPooledObjectBucket* Bucket = ObjectPools.Find(*ManagedClass);
	const FObjectPoolClassConfig* Config = FindObjectConfig(*ManagedClass);
	if (Bucket == nullptr || Config == nullptr || !Bucket->ContainsActive(Object))
	{
		return false;
	}
	if (ObjectTransitions.Contains(Object))
	{
		return false;
	}

	ObjectTransitions.Add(Object, EObjectPoolEntryState::Releasing);
	ON_SCOPE_EXIT
	{
		ObjectTransitions.Remove(Object);
	};

	InvokeObjectReleaseCallback(Object);
	if (!IsValid(Object) || ManagedObjects.Find(Object) == nullptr || !Bucket->ContainsActive(Object))
	{
		Bucket->Remove(Object);
		ManagedObjects.Remove(Object);
		ReplicatedObjectOwners.Remove(Object);
		return false;
	}
	UnregisterObjectFromReplication(Object, Config->RemoteReleasePolicy);
	DeactivateObject(Object, Config->RecoveryPolicy);
	if (Config->bRegisterAsReplicatedSubObject &&
		Config->RemoteReleasePolicy != EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica)
	{
		MoveObjectToOuter(Object, PoolOuter);
		Bucket->Remove(Object);
		ManagedObjects.Remove(Object);
		Object->MarkAsGarbage();
		ValidateAfterMutation(TEXT("DiscardReleasedNetworkObject"));
		return true;
	}
	if (!MoveObjectToOuter(Object, PoolOuter))
	{
		Bucket->Remove(Object);
		ManagedObjects.Remove(Object);
		Object->MarkAsGarbage();
		return false;
	}

	const EObjectPoolOperationResult Result = Bucket->Release(Object);
	if (Result == EObjectPoolOperationResult::Succeeded)
	{
		ValidateAfterMutation(TEXT("ReleaseObject"));
		return true;
	}

	if (Result == EObjectPoolOperationResult::CapacityReached)
	{
		Bucket->Remove(Object);
		ManagedObjects.Remove(Object);
		Object->MarkAsGarbage();
	}
	return false;
}

UActorComponent* UObjectPoolSubsystem::GetComponentFromPool(
	AActor* Owner,
	const TSubclassOf<UActorComponent> ComponentClass)
{
	check(IsInGameThread());
	UClass* Class = ComponentClass.Get();
	const UObjectPoolSettings* Settings = GetDefault<UObjectPoolSettings>();
	if (bIsDeinitializing || !bSettingsValid || !IsValid(Owner) || Owner->GetWorld() != GetWorld() || Class == nullptr ||
		!Settings->bEnabled || !IsObjectPoolRuntimeEnabled())
	{
		return nullptr;
	}
	if (!Class->IsChildOf(UActorComponent::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Cannot use class %s in the component pool."), *GetNameSafe(Class));
		return nullptr;
	}
	if (IsComponentClassTransitioning(Class))
	{
		return nullptr;
	}

	const FComponentPoolClassConfig* Config = FindComponentConfig(Class);
	if (Config == nullptr || !Config->bEnabled)
	{
		return nullptr;
	}

	FPooledObjectBucket& Bucket = FindOrAddComponentBucket(Class, *Config);
	RemoveInvalidManagedComponents();
	Bucket.RemoveInvalidObjects();
	FObjectPoolClassStats& Stats = ComponentStats.FindOrAdd(Class);

	UActorComponent* Component = Cast<UActorComponent>(Bucket.Acquire());
	if (Component != nullptr)
	{
		++Stats.PoolHits;
	}
	else
	{
		++Stats.PoolMisses;
		if (!Config->bAllowRuntimeGrowth || Bucket.NumTotal() >= Config->MaxPoolSize)
		{
			return nullptr;
		}

		Component = CreateComponent(Class);
		if (Component == nullptr || Bucket.RegisterActive(Component) != EObjectPoolOperationResult::Succeeded)
		{
			return nullptr;
		}

		ManagedComponents.Add(Component, Class);
		++Stats.Created;
		InvokeComponentCreatedCallback(Component);
	}

	if (!MoveComponentToOwner(Component, Owner))
	{
		Bucket.Remove(Component);
		ManagedComponents.Remove(Component);
		Component->MarkAsGarbage();
		return nullptr;
	}

	if (!Component->IsRegistered())
	{
		Component->RegisterComponentWithWorld(GetWorld());
	}
	if (Config->RecoveryPolicy != EObjectPoolRecoveryPolicy::BusinessCallbacksOnly)
	{
		Component->Activate(true);
		Component->SetComponentTickEnabled(true);
	}
	ActivateComponentReplication(Component, *Config);
	InvokeComponentAcquireCallback(Component);
	if (!IsValid(Component) || ManagedComponents.Find(Component) == nullptr || !Bucket.ContainsActive(Component))
	{
		Bucket.Remove(Component);
		ManagedComponents.Remove(Component);
		return nullptr;
	}
	ValidateAfterMutation(TEXT("AcquireComponent"));
	return Component;
}

bool UObjectPoolSubsystem::ReleaseComponentToPool(UActorComponent* Component)
{
	check(IsInGameThread());
	if (bIsDeinitializing || !IsValid(Component) || EnsureComponentPoolOwner() == nullptr)
	{
		return false;
	}

	TObjectPtr<UClass>* ManagedClass = ManagedComponents.Find(Component);
	if (ManagedClass == nullptr)
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Rejected unmanaged component release: %s."), *GetNameSafe(Component));
		return false;
	}

	FPooledObjectBucket* Bucket = ComponentPools.Find(*ManagedClass);
	const FComponentPoolClassConfig* Config = FindComponentConfig(*ManagedClass);
	if (Bucket == nullptr || Config == nullptr || !Bucket->ContainsActive(Component))
	{
		return false;
	}
	if (ComponentTransitions.Contains(Component))
	{
		return false;
	}

	ComponentTransitions.Add(Component, EObjectPoolEntryState::Releasing);
	ON_SCOPE_EXIT
	{
		ComponentTransitions.Remove(Component);
	};

	InvokeComponentReleaseCallback(Component);
	if (!IsValid(Component) || ManagedComponents.Find(Component) == nullptr || !Bucket->ContainsActive(Component))
	{
		Bucket->Remove(Component);
		ManagedComponents.Remove(Component);
		return false;
	}
	DeactivateComponentReplication(Component, Config->RemoteReleasePolicy);
	DeactivateComponent(Component, Config->RecoveryPolicy);
	if (Config->bReplicateComponent &&
		Config->RemoteReleasePolicy != EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica)
	{
		MoveComponentToOwner(Component, ComponentPoolOwner);
		Bucket->Remove(Component);
		ManagedComponents.Remove(Component);
		Component->MarkAsGarbage();
		ValidateAfterMutation(TEXT("DiscardReleasedNetworkComponent"));
		return true;
	}

	if (!MoveComponentToOwner(Component, ComponentPoolOwner))
	{
		Bucket->Remove(Component);
		ManagedComponents.Remove(Component);
		Component->MarkAsGarbage();
		return false;
	}

	const EObjectPoolOperationResult Result = Bucket->Release(Component);
	if (Result == EObjectPoolOperationResult::Succeeded)
	{
		ValidateAfterMutation(TEXT("ReleaseComponent"));
		return true;
	}

	if (Result == EObjectPoolOperationResult::CapacityReached)
	{
		Bucket->Remove(Component);
		ManagedComponents.Remove(Component);
		Component->MarkAsGarbage();
	}
	return false;
}

EObjectPoolEntryState UObjectPoolSubsystem::GetActorPoolState(const AActor* Actor) const
{
	if (Actor == nullptr)
	{
		return EObjectPoolEntryState::Unmanaged;
	}
	if (const EObjectPoolEntryState* Transition = ActorTransitions.Find(Actor))
	{
		return *Transition;
	}

	const TObjectPtr<UClass>* ManagedClass = ManagedActors.Find(Actor);
	const FPooledObjectBucket* Bucket = ManagedClass != nullptr ? ActorPools.Find(*ManagedClass) : nullptr;
	return Bucket != nullptr ? Bucket->GetState(Actor) : EObjectPoolEntryState::Unmanaged;
}

EObjectPoolEntryState UObjectPoolSubsystem::GetObjectPoolState(const UObject* Object) const
{
	if (Object == nullptr)
	{
		return EObjectPoolEntryState::Unmanaged;
	}
	if (const EObjectPoolEntryState* Transition = ObjectTransitions.Find(Object))
	{
		return *Transition;
	}

	const TObjectPtr<UClass>* ManagedClass = ManagedObjects.Find(Object);
	const FPooledObjectBucket* Bucket = ManagedClass != nullptr ? ObjectPools.Find(*ManagedClass) : nullptr;
	return Bucket != nullptr ? Bucket->GetState(Object) : EObjectPoolEntryState::Unmanaged;
}

EObjectPoolEntryState UObjectPoolSubsystem::GetComponentPoolState(const UActorComponent* Component) const
{
	if (Component == nullptr)
	{
		return EObjectPoolEntryState::Unmanaged;
	}
	if (const EObjectPoolEntryState* Transition = ComponentTransitions.Find(Component))
	{
		return *Transition;
	}
	const TObjectPtr<UClass>* ManagedClass = ManagedComponents.Find(Component);
	const FPooledObjectBucket* Bucket = ManagedClass != nullptr ? ComponentPools.Find(*ManagedClass) : nullptr;
	return Bucket != nullptr ? Bucket->GetState(Component) : EObjectPoolEntryState::Unmanaged;
}

FObjectPoolClassStats UObjectPoolSubsystem::GetActorPoolStats(const TSubclassOf<AActor> ActorClass) const
{
	FObjectPoolClassStats Result;
	UClass* Class = ActorClass.Get();
	if (const FObjectPoolClassStats* StoredStats = ActorStats.Find(Class))
	{
		Result = *StoredStats;
	}

	if (const FPooledObjectBucket* Bucket = ActorPools.Find(Class))
	{
		Result.Active = Bucket->NumActive();
		Result.Inactive = Bucket->NumInactive();
	}
	return Result;
}

FObjectPoolClassStats UObjectPoolSubsystem::GetObjectPoolStats(const TSubclassOf<UObject> ObjectClass) const
{
	FObjectPoolClassStats Result;
	UClass* Class = ObjectClass.Get();
	if (const FObjectPoolClassStats* StoredStats = ObjectStats.Find(Class))
	{
		Result = *StoredStats;
	}
	if (const FPooledObjectBucket* Bucket = ObjectPools.Find(Class))
	{
		Result.Active = Bucket->NumActive();
		Result.Inactive = Bucket->NumInactive();
	}
	return Result;
}

FObjectPoolClassStats UObjectPoolSubsystem::GetComponentPoolStats(const TSubclassOf<UActorComponent> ComponentClass) const
{
	FObjectPoolClassStats Result;
	UClass* Class = ComponentClass.Get();
	if (const FObjectPoolClassStats* StoredStats = ComponentStats.Find(Class))
	{
		Result = *StoredStats;
	}
	if (const FPooledObjectBucket* Bucket = ComponentPools.Find(Class))
	{
		Result.Active = Bucket->NumActive();
		Result.Inactive = Bucket->NumInactive();
	}
	return Result;
}

TArray<FObjectPoolClassSnapshot> UObjectPoolSubsystem::GetActorPoolSnapshots() const
{
	TArray<FObjectPoolClassSnapshot> Results;
	Results.Reserve(ActorPools.Num());
	for (const TPair<TObjectPtr<UClass>, FPooledObjectBucket>& Pair : ActorPools)
	{
		FObjectPoolClassSnapshot& Snapshot = Results.AddDefaulted_GetRef();
		Snapshot.ObjectClass = Pair.Key;
		Snapshot.MaxPoolSize = Pair.Value.GetMaxPoolSize();
		if (const FObjectPoolClassStats* StoredStats = ActorStats.Find(Pair.Key))
		{
			Snapshot.Stats = *StoredStats;
		}
		Snapshot.Stats.Active = Pair.Value.NumActive();
		Snapshot.Stats.Inactive = Pair.Value.NumInactive();
	}

	Results.Sort([](const FObjectPoolClassSnapshot& Left, const FObjectPoolClassSnapshot& Right)
	{
		return GetNameSafe(Left.ObjectClass) < GetNameSafe(Right.ObjectClass);
	});
	return Results;
}

TArray<FObjectPoolClassSnapshot> UObjectPoolSubsystem::GetObjectPoolSnapshots() const
{
	TArray<FObjectPoolClassSnapshot> Results;
	Results.Reserve(ObjectPools.Num());
	for (const TPair<TObjectPtr<UClass>, FPooledObjectBucket>& Pair : ObjectPools)
	{
		FObjectPoolClassSnapshot& Snapshot = Results.AddDefaulted_GetRef();
		Snapshot.ObjectClass = Pair.Key;
		Snapshot.MaxPoolSize = Pair.Value.GetMaxPoolSize();
		if (const FObjectPoolClassStats* StoredStats = ObjectStats.Find(Pair.Key))
		{
			Snapshot.Stats = *StoredStats;
		}
		Snapshot.Stats.Active = Pair.Value.NumActive();
		Snapshot.Stats.Inactive = Pair.Value.NumInactive();
	}

	Results.Sort([](const FObjectPoolClassSnapshot& Left, const FObjectPoolClassSnapshot& Right)
	{
		return GetNameSafe(Left.ObjectClass) < GetNameSafe(Right.ObjectClass);
	});
	return Results;
}

TArray<FObjectPoolClassSnapshot> UObjectPoolSubsystem::GetComponentPoolSnapshots() const
{
	TArray<FObjectPoolClassSnapshot> Results;
	Results.Reserve(ComponentPools.Num());
	for (const TPair<TObjectPtr<UClass>, FPooledObjectBucket>& Pair : ComponentPools)
	{
		FObjectPoolClassSnapshot& Snapshot = Results.AddDefaulted_GetRef();
		Snapshot.ObjectClass = Pair.Key;
		Snapshot.MaxPoolSize = Pair.Value.GetMaxPoolSize();
		if (const FObjectPoolClassStats* StoredStats = ComponentStats.Find(Pair.Key))
		{
			Snapshot.Stats = *StoredStats;
		}
		Snapshot.Stats.Active = Pair.Value.NumActive();
		Snapshot.Stats.Inactive = Pair.Value.NumInactive();
	}
	Results.Sort([](const FObjectPoolClassSnapshot& Left, const FObjectPoolClassSnapshot& Right)
	{
		return GetNameSafe(Left.ObjectClass) < GetNameSafe(Right.ObjectClass);
	});
	return Results;
}

FString UObjectPoolSubsystem::BuildDebugReport() const
{
	FString Report = FString::Printf(
		TEXT("ObjectPool World=%s Mode=%s Enabled=%s Prewarm(Actor=%s UObject=%s Component=%s)\n"),
		*GetNameSafe(GetWorld()),
		*CurrentModeTag.ToString(),
		IsObjectPoolRuntimeEnabled() ? TEXT("true") : TEXT("false"),
		bActorPrewarmComplete ? TEXT("complete") : TEXT("pending"),
		bObjectPrewarmComplete ? TEXT("complete") : TEXT("pending"),
		bComponentPrewarmComplete ? TEXT("complete") : TEXT("pending"));

	const TArray<FObjectPoolClassSnapshot> Snapshots = GetActorPoolSnapshots();
	if (Snapshots.IsEmpty())
	{
		Report += TEXT("  Actor pools: none\n");
	}
	else for (const FObjectPoolClassSnapshot& Snapshot : Snapshots)
	{
		const int32 Requests = Snapshot.Stats.PoolHits + Snapshot.Stats.PoolMisses;
		const double HitRate = Requests > 0 ? 100.0 * Snapshot.Stats.PoolHits / Requests : 0.0;
		Report += FString::Printf(
			TEXT("  %s Active=%d Inactive=%d Max=%d Created=%d Preallocated=%d Hits=%d Misses=%d HitRate=%.1f%%\n"),
			*GetNameSafe(Snapshot.ObjectClass), Snapshot.Stats.Active, Snapshot.Stats.Inactive,
			Snapshot.MaxPoolSize, Snapshot.Stats.Created, Snapshot.Stats.Preallocated,
			Snapshot.Stats.PoolHits, Snapshot.Stats.PoolMisses, HitRate);
	}

	const TArray<FObjectPoolClassSnapshot> ObjectSnapshots = GetObjectPoolSnapshots();
	if (ObjectSnapshots.IsEmpty())
	{
		Report += TEXT("  UObject pools: none\n");
	}
	else
	{
		for (const FObjectPoolClassSnapshot& Snapshot : ObjectSnapshots)
		{
			const int32 Requests = Snapshot.Stats.PoolHits + Snapshot.Stats.PoolMisses;
			const double HitRate = Requests > 0 ? 100.0 * Snapshot.Stats.PoolHits / Requests : 0.0;
			Report += FString::Printf(
				TEXT("  UObject %s Active=%d Inactive=%d Max=%d Created=%d Preallocated=%d Hits=%d Misses=%d HitRate=%.1f%%\n"),
				*GetNameSafe(Snapshot.ObjectClass), Snapshot.Stats.Active, Snapshot.Stats.Inactive,
				Snapshot.MaxPoolSize, Snapshot.Stats.Created, Snapshot.Stats.Preallocated,
				Snapshot.Stats.PoolHits, Snapshot.Stats.PoolMisses, HitRate);
		}
	}

	const TArray<FObjectPoolClassSnapshot> ComponentSnapshots = GetComponentPoolSnapshots();
	if (ComponentSnapshots.IsEmpty())
	{
		Report += TEXT("  Component pools: none\n");
	}
	else
	{
		for (const FObjectPoolClassSnapshot& Snapshot : ComponentSnapshots)
		{
			const int32 Requests = Snapshot.Stats.PoolHits + Snapshot.Stats.PoolMisses;
			const double HitRate = Requests > 0 ? 100.0 * Snapshot.Stats.PoolHits / Requests : 0.0;
			Report += FString::Printf(
				TEXT("  Component %s Active=%d Inactive=%d Max=%d Created=%d Preallocated=%d Hits=%d Misses=%d HitRate=%.1f%%\n"),
				*GetNameSafe(Snapshot.ObjectClass), Snapshot.Stats.Active, Snapshot.Stats.Inactive,
				Snapshot.MaxPoolSize, Snapshot.Stats.Created, Snapshot.Stats.Preallocated,
				Snapshot.Stats.PoolHits, Snapshot.Stats.PoolMisses, HitRate);
		}
	}
	return Report;
}

bool UObjectPoolSubsystem::Validate(FString* OutError) const
{
	for (const TPair<TObjectPtr<UClass>, FPooledObjectBucket>& Pair : ActorPools)
	{
		FString BucketError;
		if (!Pair.Value.Validate(&BucketError))
		{
			if (OutError != nullptr)
			{
				*OutError = FString::Printf(TEXT("%s: %s"), *GetNameSafe(Pair.Key), *BucketError);
			}
			return false;
		}
		TArray<UObject*> BucketObjects;
		Pair.Value.GetObjects(BucketObjects);
		for (UObject* Object : BucketObjects)
		{
			AActor* Actor = Cast<AActor>(Object);
			const TObjectPtr<UClass>* ManagedClass = Actor != nullptr ? ManagedActors.Find(Actor) : nullptr;
			if (ManagedClass == nullptr || *ManagedClass != Pair.Key)
			{
				if (OutError != nullptr)
				{
					*OutError = TEXT("Actor bucket contains an entry missing from the managed ownership map.");
				}
				return false;
			}
		}
	}

	for (const TPair<TObjectPtr<UClass>, FPooledObjectBucket>& Pair : ObjectPools)
	{
		FString BucketError;
		if (!Pair.Value.Validate(&BucketError))
		{
			if (OutError != nullptr)
			{
				*OutError = FString::Printf(TEXT("UObject %s: %s"), *GetNameSafe(Pair.Key), *BucketError);
			}
			return false;
		}
		TArray<UObject*> BucketObjects;
		Pair.Value.GetObjects(BucketObjects);
		for (UObject* Object : BucketObjects)
		{
			const TObjectPtr<UClass>* ManagedClass = ManagedObjects.Find(Object);
			if (ManagedClass == nullptr || *ManagedClass != Pair.Key)
			{
				if (OutError != nullptr)
				{
					*OutError = TEXT("UObject bucket contains an entry missing from the managed ownership map.");
				}
				return false;
			}
		}
	}

	for (const TPair<TObjectPtr<UClass>, FPooledObjectBucket>& Pair : ComponentPools)
	{
		FString BucketError;
		if (!Pair.Value.Validate(&BucketError))
		{
			if (OutError != nullptr)
			{
				*OutError = FString::Printf(TEXT("Component %s: %s"), *GetNameSafe(Pair.Key), *BucketError);
			}
			return false;
		}
		TArray<UObject*> BucketObjects;
		Pair.Value.GetObjects(BucketObjects);
		for (UObject* Object : BucketObjects)
		{
			UActorComponent* Component = Cast<UActorComponent>(Object);
			const TObjectPtr<UClass>* ManagedClass = Component != nullptr ? ManagedComponents.Find(Component) : nullptr;
			if (ManagedClass == nullptr || *ManagedClass != Pair.Key)
			{
				if (OutError != nullptr)
				{
					*OutError = TEXT("Component bucket contains an entry missing from the managed ownership map.");
				}
				return false;
			}
		}
	}

	for (const TPair<TObjectPtr<AActor>, TObjectPtr<UClass>>& Pair : ManagedActors)
	{
		const FPooledObjectBucket* Bucket = ActorPools.Find(Pair.Value);
		if (!IsValid(Pair.Key) || Bucket == nullptr || Bucket->GetState(Pair.Key) == EObjectPoolEntryState::Unmanaged)
		{
			if (OutError != nullptr)
			{
				*OutError = TEXT("Managed actor ownership does not match its bucket.");
			}
			return false;
		}
	}

	for (const TPair<TObjectPtr<UObject>, TObjectPtr<UClass>>& Pair : ManagedObjects)
	{
		const FPooledObjectBucket* Bucket = ObjectPools.Find(Pair.Value);
		if (!IsValid(Pair.Key) || Bucket == nullptr || Bucket->GetState(Pair.Key) == EObjectPoolEntryState::Unmanaged)
		{
			if (OutError != nullptr)
			{
				*OutError = TEXT("Managed UObject ownership does not match its bucket.");
			}
			return false;
		}

		if (Bucket->ContainsInactive(Pair.Key) && Pair.Key->GetOuter() != PoolOuter)
		{
			if (OutError != nullptr)
			{
				*OutError = TEXT("Inactive UObject is not parented to PoolOuter.");
			}
			return false;
		}
	}

	for (const TPair<TObjectPtr<UActorComponent>, TObjectPtr<UClass>>& Pair : ManagedComponents)
	{
		const FPooledObjectBucket* Bucket = ComponentPools.Find(Pair.Value);
		if (!IsValid(Pair.Key) || Bucket == nullptr || Bucket->GetState(Pair.Key) == EObjectPoolEntryState::Unmanaged)
		{
			if (OutError != nullptr)
			{
				*OutError = TEXT("Managed component ownership does not match its bucket.");
			}
			return false;
		}

		if (Bucket->ContainsInactive(Pair.Key) &&
			(Pair.Key->GetOwner() != ComponentPoolOwner || Pair.Key->IsRegistered()))
		{
			if (OutError != nullptr)
			{
				*OutError = TEXT("Inactive component has an invalid owner or remains registered.");
			}
			return false;
		}
	}

	for (const TPair<TObjectPtr<UObject>, FObjectPoolReplicatedOwnerBinding>& Pair : ReplicatedObjectOwners)
	{
		const FObjectPoolReplicatedOwnerBinding& Binding = Pair.Value;
		const FPooledObjectBucket* Bucket = ManagedObjects.Contains(Pair.Key)
			? ObjectPools.Find(ManagedObjects.FindChecked(Pair.Key))
			: nullptr;
		const bool bRegistered = IsValid(Binding.Component)
			? IsValid(Binding.Actor) && Binding.Actor->IsActorComponentReplicatedSubObjectRegistered(Binding.Component, Pair.Key)
			: IsValid(Binding.Actor) && Binding.Actor->IsReplicatedSubObjectRegistered(Pair.Key);
		if (!IsValid(Pair.Key) || Bucket == nullptr || !Bucket->ContainsActive(Pair.Key) || !bRegistered)
		{
			if (OutError != nullptr)
			{
				*OutError = TEXT("Replicated UObject binding is stale or inconsistent.");
			}
			return false;
		}
	}

	if (OutError != nullptr)
	{
		OutError->Reset();
	}
	return true;
}

const FActorPoolClassConfig* UObjectPoolSubsystem::FindActorConfig(UClass* ActorClass) const
{
	const FPoolModeConfig* ModeConfig = FindCurrentModeConfig();
	if (ModeConfig == nullptr)
	{
		return nullptr;
	}

	const FSoftObjectPath ClassPath(ActorClass);
	return ModeConfig->ActorPools.FindByPredicate([ActorClass, &ClassPath](const FActorPoolClassConfig& Config)
	{
		return Config.ActorClass.Get() == ActorClass || Config.ActorClass.ToSoftObjectPath() == ClassPath;
	});
}

const FObjectPoolClassConfig* UObjectPoolSubsystem::FindObjectConfig(UClass* ObjectClass) const
{
	const FPoolModeConfig* ModeConfig = FindCurrentModeConfig();
	if (ModeConfig == nullptr)
	{
		return nullptr;
	}

	const FSoftObjectPath ClassPath(ObjectClass);
	return ModeConfig->ObjectPools.FindByPredicate([ObjectClass, &ClassPath](const FObjectPoolClassConfig& Config)
	{
		return Config.ObjectClass.ResolveClass() == ObjectClass || Config.ObjectClass == ClassPath;
	});
}

const FComponentPoolClassConfig* UObjectPoolSubsystem::FindComponentConfig(UClass* ComponentClass) const
{
	const FPoolModeConfig* ModeConfig = FindCurrentModeConfig();
	if (ModeConfig == nullptr)
	{
		return nullptr;
	}

	const FSoftObjectPath ClassPath(ComponentClass);
	return ModeConfig->ComponentPools.FindByPredicate([ComponentClass, &ClassPath](const FComponentPoolClassConfig& Config)
	{
		return Config.ComponentClass.ResolveClass() == ComponentClass || Config.ComponentClass == ClassPath;
	});
}

const FPoolModeConfig* UObjectPoolSubsystem::FindCurrentModeConfig() const
{
	const UObjectPoolSettings* Settings = GetDefault<UObjectPoolSettings>();
	if (const FPoolModeConfig* ModeConfig = Settings->ModeConfigs.Find(CurrentModeTag))
	{
		return ModeConfig;
	}

	if (CurrentModeTag != Settings->DefaultModeTag)
	{
		if (const FPoolModeConfig* DefaultTaggedConfig = Settings->ModeConfigs.Find(Settings->DefaultModeTag))
		{
			return DefaultTaggedConfig;
		}
	}

	return &Settings->DefaultModeConfig;
}

FGameplayTag UObjectPoolSubsystem::ResolveCurrentModeTag() const
{
	const UObjectPoolSettings* Settings = GetDefault<UObjectPoolSettings>();
	UWorld* World = GetWorld();
	AGameStateBase* GameState = World != nullptr ? World->GetGameState() : nullptr;
	if (GameState == nullptr || Settings->ModeTagProviderFunctionName.IsNone())
	{
		return Settings->DefaultModeTag;
	}

	UFunction* Function = GameState->FindFunction(Settings->ModeTagProviderFunctionName);
	FProperty* ReturnProperty = Function != nullptr ? Function->GetReturnProperty() : nullptr;
	FStructProperty* StructReturn = CastField<FStructProperty>(ReturnProperty);
	if (Function == nullptr || Function->NumParms != 1 || StructReturn == nullptr || StructReturn->Struct != FGameplayTag::StaticStruct())
	{
		return Settings->DefaultModeTag;
	}

	FStructOnScope Parameters(Function);
	GameState->ProcessEvent(Function, Parameters.GetStructMemory());
	const FGameplayTag* ReturnedTag = StructReturn->ContainerPtrToValuePtr<FGameplayTag>(Parameters.GetStructMemory());
	return ReturnedTag != nullptr && ReturnedTag->IsValid() ? *ReturnedTag : Settings->DefaultModeTag;
}

bool UObjectPoolSubsystem::HasAnyActiveEntries() const
{
	for (const TPair<TObjectPtr<UClass>, FPooledObjectBucket>& Pair : ActorPools)
	{
		if (Pair.Value.NumActive() > 0)
		{
			return true;
		}
	}
	for (const TPair<TObjectPtr<UClass>, FPooledObjectBucket>& Pair : ObjectPools)
	{
		if (Pair.Value.NumActive() > 0)
		{
			return true;
		}
	}
	for (const TPair<TObjectPtr<UClass>, FPooledObjectBucket>& Pair : ComponentPools)
	{
		if (Pair.Value.NumActive() > 0)
		{
			return true;
		}
	}
	return false;
}

bool UObjectPoolSubsystem::IsActorClassTransitioning(UClass* ActorClass) const
{
	for (const TPair<TObjectPtr<AActor>, EObjectPoolEntryState>& Pair : ActorTransitions)
	{
		if (ManagedActors.FindRef(Pair.Key) == ActorClass)
		{
			return true;
		}
	}
	return false;
}

bool UObjectPoolSubsystem::IsObjectClassTransitioning(UClass* ObjectClass) const
{
	for (const TPair<TObjectPtr<UObject>, EObjectPoolEntryState>& Pair : ObjectTransitions)
	{
		if (ManagedObjects.FindRef(Pair.Key) == ObjectClass)
		{
			return true;
		}
	}
	return false;
}

bool UObjectPoolSubsystem::IsComponentClassTransitioning(UClass* ComponentClass) const
{
	for (const TPair<TObjectPtr<UActorComponent>, EObjectPoolEntryState>& Pair : ComponentTransitions)
	{
		if (ManagedComponents.FindRef(Pair.Key) == ComponentClass)
		{
			return true;
		}
	}
	return false;
}

void UObjectPoolSubsystem::ResetPoolsForModeSwitch()
{
	if (ActorPrewarmHandle.IsValid())
	{
		ActorPrewarmHandle->CancelHandle();
		ActorPrewarmHandle.Reset();
	}
	if (ObjectPrewarmHandle.IsValid())
	{
		ObjectPrewarmHandle->CancelHandle();
		ObjectPrewarmHandle.Reset();
	}
	if (ComponentPrewarmHandle.IsValid())
	{
		ComponentPrewarmHandle->CancelHandle();
		ComponentPrewarmHandle.Reset();
	}

	for (const TPair<TObjectPtr<AActor>, TObjectPtr<UClass>>& Pair : ManagedActors)
	{
		if (IsValid(Pair.Key))
		{
			Pair.Key->Destroy();
		}
	}
	TArray<TObjectPtr<UObject>> ReplicatedObjects;
	ReplicatedObjectOwners.GenerateKeyArray(ReplicatedObjects);
	for (UObject* Object : ReplicatedObjects)
	{
		UnregisterObjectFromReplication(Object, EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica);
	}
	for (const TPair<TObjectPtr<UObject>, TObjectPtr<UClass>>& Pair : ManagedObjects)
	{
		if (IsValid(Pair.Key))
		{
			Pair.Key->MarkAsGarbage();
		}
	}

	ManagedActors.Reset();
	ActorPools.Reset();
	ActorStats.Reset();
	ActorTransitions.Reset();
	ManagedObjects.Reset();
	ObjectPools.Reset();
	ObjectStats.Reset();
	ObjectTransitions.Reset();
	ReplicatedObjectOwners.Reset();
	ManagedComponents.Reset();
	ComponentPools.Reset();
	ComponentStats.Reset();
	ComponentTransitions.Reset();
	if (IsValid(ComponentPoolOwner))
	{
		ComponentPoolOwner->Destroy();
	}
	ComponentPoolOwner = nullptr;
}

FPooledObjectBucket& UObjectPoolSubsystem::FindOrAddActorBucket(UClass* ActorClass, const FActorPoolClassConfig& Config)
{
	FPooledObjectBucket& Bucket = ActorPools.FindOrAdd(ActorClass);
	Bucket.Configure(Config.MaxPoolSize);
	return Bucket;
}

FPooledObjectBucket& UObjectPoolSubsystem::FindOrAddObjectBucket(UClass* ObjectClass, const FObjectPoolClassConfig& Config)
{
	FPooledObjectBucket& Bucket = ObjectPools.FindOrAdd(ObjectClass);
	Bucket.Configure(Config.MaxPoolSize);
	return Bucket;
}

UObject* UObjectPoolSubsystem::CreateObject(UClass* ObjectClass)
{
	return PoolOuter != nullptr ? NewObject<UObject>(PoolOuter, ObjectClass, NAME_None, RF_Transient) : nullptr;
}

bool UObjectPoolSubsystem::MoveObjectToOuter(UObject* Object, UObject* NewOuter) const
{
	if (!IsValid(Object) || NewOuter == nullptr)
	{
		return false;
	}
	if (Object->GetOuter() == NewOuter)
	{
		return true;
	}
	return Object->Rename(nullptr, NewOuter, REN_DontCreateRedirectors | REN_NonTransactional);
}

void UObjectPoolSubsystem::InvokeObjectCreatedCallback(UObject* Object) const
{
	if (Object->GetClass()->ImplementsInterface(UPoolableObjectInterface::StaticClass()))
	{
		IPoolableObjectInterface::Execute_OnObjectPoolCreated(Object);
	}
}

void UObjectPoolSubsystem::InvokeObjectAcquireCallback(UObject* Object) const
{
	if (Object->GetClass()->ImplementsInterface(UPoolableObjectInterface::StaticClass()))
	{
		IPoolableObjectInterface::Execute_OnObjectAcquired(Object);
	}
}

void UObjectPoolSubsystem::InvokeObjectReleaseCallback(UObject* Object) const
{
	if (Object->GetClass()->ImplementsInterface(UPoolableObjectInterface::StaticClass()))
	{
		IPoolableObjectInterface::Execute_OnObjectReleased(Object);
	}
}

void UObjectPoolSubsystem::DeactivateObject(
	UObject* Object,
	const EObjectPoolRecoveryPolicy RecoveryPolicy) const
{
	if (!IsValid(Object) || RecoveryPolicy == EObjectPoolRecoveryPolicy::BusinessCallbacksOnly)
	{
		return;
	}

	GetWorld()->GetTimerManager().ClearAllTimersForObject(Object);
	if (RecoveryPolicy == EObjectPoolRecoveryPolicy::Full)
	{
		GetWorld()->GetLatentActionManager().RemoveActionsForObject(Object);
	}
}

AActor* UObjectPoolSubsystem::EnsureComponentPoolOwner()
{
	if (IsValid(ComponentPoolOwner))
	{
		return ComponentPoolOwner;
	}

	UWorld* World = GetWorld();
	if (World == nullptr || bIsDeinitializing)
	{
		return nullptr;
	}

	FActorSpawnParameters Parameters;
	Parameters.Name = MakeUniqueObjectName(World, AActor::StaticClass(), TEXT("ObjectPoolComponentOwner"));
	Parameters.ObjectFlags |= RF_Transient;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ComponentPoolOwner = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Parameters);
	if (ComponentPoolOwner != nullptr)
	{
		ComponentPoolOwner->SetActorHiddenInGame(true);
		ComponentPoolOwner->SetActorEnableCollision(false);
		ComponentPoolOwner->SetActorTickEnabled(false);
	}
	return ComponentPoolOwner;
}

FPooledObjectBucket& UObjectPoolSubsystem::FindOrAddComponentBucket(
	UClass* ComponentClass,
	const FComponentPoolClassConfig& Config)
{
	FPooledObjectBucket& Bucket = ComponentPools.FindOrAdd(ComponentClass);
	Bucket.Configure(Config.MaxPoolSize);
	return Bucket;
}

UActorComponent* UObjectPoolSubsystem::CreateComponent(UClass* ComponentClass)
{
	AActor* PoolOwner = EnsureComponentPoolOwner();
	return PoolOwner != nullptr
		? NewObject<UActorComponent>(PoolOwner, ComponentClass, NAME_None, RF_Transient)
		: nullptr;
}

bool UObjectPoolSubsystem::MoveComponentToOwner(UActorComponent* Component, AActor* NewOwner)
{
	if (!IsValid(Component) || !IsValid(NewOwner))
	{
		return false;
	}

	AActor* PreviousOwner = Component->GetOwner();
	if (PreviousOwner == NewOwner)
	{
		return true;
	}

	if (Component->IsRegistered())
	{
		Component->UnregisterComponent();
	}
	if (PreviousOwner != nullptr && PreviousOwner != ComponentPoolOwner)
	{
		PreviousOwner->RemoveInstanceComponent(Component);
	}

	if (!Component->Rename(nullptr, NewOwner, REN_DontCreateRedirectors | REN_NonTransactional))
	{
		return false;
	}

	if (NewOwner != ComponentPoolOwner)
	{
		NewOwner->AddInstanceComponent(Component);
	}
	return true;
}

void UObjectPoolSubsystem::InvokeComponentCreatedCallback(UActorComponent* Component) const
{
	if (Component->GetClass()->ImplementsInterface(UPoolableComponentInterface::StaticClass()))
	{
		IPoolableComponentInterface::Execute_OnComponentPoolCreated(Component);
	}
}

void UObjectPoolSubsystem::InvokeComponentAcquireCallback(UActorComponent* Component) const
{
	if (Component->GetClass()->ImplementsInterface(UPoolableComponentInterface::StaticClass()))
	{
		IPoolableComponentInterface::Execute_OnComponentAcquired(Component);
	}
}

void UObjectPoolSubsystem::InvokeComponentReleaseCallback(UActorComponent* Component) const
{
	if (Component->GetClass()->ImplementsInterface(UPoolableComponentInterface::StaticClass()))
	{
		IPoolableComponentInterface::Execute_OnComponentReleased(Component);
	}
}

void UObjectPoolSubsystem::DeactivateComponent(
	UActorComponent* Component,
	const EObjectPoolRecoveryPolicy RecoveryPolicy)
{
	if (!IsValid(Component))
	{
		return;
	}

	if (RecoveryPolicy != EObjectPoolRecoveryPolicy::BusinessCallbacksOnly)
	{
		GetWorld()->GetTimerManager().ClearAllTimersForObject(Component);
		Component->SetComponentTickEnabled(false);
		Component->Deactivate();
	}
	if (RecoveryPolicy == EObjectPoolRecoveryPolicy::Full)
	{
		GetWorld()->GetLatentActionManager().RemoveActionsForObject(Component);
	}

	// These operations are mandatory for safe ownership transfer, regardless of recovery policy.
	if (USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
	{
		SceneComponent->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	}
	if (Component->IsRegistered())
	{
		Component->UnregisterComponent();
	}
}

AActor* UObjectPoolSubsystem::CreateActor(
	UClass* ActorClass,
	const FTransform& Transform,
	AActor* Owner,
	APawn* Instigator)
{
	AActor* Actor = GetWorld()->SpawnActorDeferred<AActor>(
		ActorClass,
		Transform,
		Owner,
		Instigator,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Actor != nullptr)
	{
		Actor->FinishSpawning(Transform);
	}
	return Actor;
}

bool UObjectPoolSubsystem::ActivateActor(AActor* Actor, const FActorPoolAcquireContext& Context)
{
	if (!IsValid(Actor))
	{
		return false;
	}
	Actor->SetActorTransform(Context.Transform, false, nullptr, ETeleportType::TeleportPhysics);
	Actor->SetOwner(Context.Owner);
	Actor->SetInstigator(Context.Instigator);
	Actor->SetActorHiddenInGame(false);
	Actor->SetActorEnableCollision(true);
	Actor->SetActorTickEnabled(true);
	InvokeAcquireCallback(Actor, Context);
	if (!IsValid(Actor))
	{
		return false;
	}
	PrepareActorNetworkAcquire(Actor, Context);
	return IsValid(Actor);
}

void UObjectPoolSubsystem::DeactivateActor(AActor* Actor, const EObjectPoolRecoveryPolicy RecoveryPolicy)
{
	if (RecoveryPolicy != EObjectPoolRecoveryPolicy::BusinessCallbacksOnly)
	{
		GetWorld()->GetTimerManager().ClearAllTimersForObject(Actor);
		Actor->SetActorTickEnabled(false);
		Actor->SetActorEnableCollision(false);
		Actor->SetActorHiddenInGame(true);
	}

	if (RecoveryPolicy == EObjectPoolRecoveryPolicy::Full)
	{
		GetWorld()->GetLatentActionManager().RemoveActionsForObject(Actor);
		Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	}

	Actor->SetOwner(nullptr);
	Actor->SetInstigator(nullptr);
	PrepareActorNetworkRelease(Actor);
}

UPooledActorNetworkStateComponent* UObjectPoolSubsystem::EnsureActorNetworkStateComponent(AActor* Actor)
{
	if (!IsObjectPoolNetworkSupportEnabled() || !IsValid(Actor) || !Actor->GetIsReplicated() || !Actor->HasAuthority())
	{
		return nullptr;
	}

	if (UPooledActorNetworkStateComponent* Existing = Actor->FindComponentByClass<UPooledActorNetworkStateComponent>())
	{
		return Existing;
	}

	UPooledActorNetworkStateComponent* Component = NewObject<UPooledActorNetworkStateComponent>(
		Actor,
		UPooledActorNetworkStateComponent::StaticClass(),
		TEXT("PooledActorNetworkState"),
		RF_Transient);
	if (Component != nullptr)
	{
		Actor->AddInstanceComponent(Component);
		Component->SetIsReplicated(true);
		Component->RegisterComponent();
	}
	return Component;
}

void UObjectPoolSubsystem::PrepareActorNetworkAcquire(AActor* Actor, const FActorPoolAcquireContext& Context)
{
	UPooledActorNetworkStateComponent* NetworkState = EnsureActorNetworkStateComponent(Actor);
	if (NetworkState == nullptr)
	{
		return;
	}

	Actor->SetNetDormancy(DORM_Awake);
	Actor->FlushNetDormancy();
	NetworkState->SetPoolAcquiredFromServer(Context);
	Actor->ForceNetUpdate();
}

void UObjectPoolSubsystem::PrepareActorNetworkRelease(AActor* Actor)
{
	UPooledActorNetworkStateComponent* NetworkState = EnsureActorNetworkStateComponent(Actor);
	if (NetworkState == nullptr)
	{
		return;
	}

	Actor->SetNetDormancy(DORM_Awake);
	Actor->FlushNetDormancy();
	NetworkState->SetPoolActiveFromServer(false);
	Actor->ForceNetUpdate();

	TWeakObjectPtr<AActor> WeakActor(Actor);
	TWeakObjectPtr<UPooledActorNetworkStateComponent> WeakNetworkState(NetworkState);
	GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakActor, WeakNetworkState]()
	{
		AActor* PendingActor = WeakActor.Get();
		UPooledActorNetworkStateComponent* PendingState = WeakNetworkState.Get();
		if (PendingActor != nullptr && PendingState != nullptr && !PendingState->IsPoolActive())
		{
			PendingActor->SetNetDormancy(DORM_DormantAll);
		}
	}));
}

bool UObjectPoolSubsystem::RefreshSettingsValidity()
{
	FString SettingsError;
	bSettingsValid = GetDefault<UObjectPoolSettings>()->ValidateSettings(&SettingsError);
	if (!bSettingsValid)
	{
		UE_LOG(LogObjectPool, Error, TEXT("Object-pool configuration is invalid; pooling is disabled: %s"), *SettingsError);
	}
	return bSettingsValid;
}

void UObjectPoolSubsystem::RegisterObjectForReplication(
	UObject* Object,
	UObject* RequestedOuter,
	const FObjectPoolClassConfig& Config)
{
	if (!Config.bRegisterAsReplicatedSubObject || !IsObjectPoolNetworkSupportEnabled() ||
		!IsValid(Object) || !Object->IsSupportedForNetworking())
	{
		return;
	}

	UActorComponent* OwnerComponent = Cast<UActorComponent>(RequestedOuter);
	if (OwnerComponent == nullptr)
	{
		OwnerComponent = RequestedOuter->GetTypedOuter<UActorComponent>();
	}
	AActor* OwnerActor = OwnerComponent != nullptr ? OwnerComponent->GetOwner() : Cast<AActor>(RequestedOuter);
	if (OwnerActor == nullptr)
	{
		OwnerActor = RequestedOuter->GetTypedOuter<AActor>();
	}

	if (!IsValid(OwnerActor) || !OwnerActor->HasAuthority() || !OwnerActor->GetIsReplicated())
	{
		return;
	}

	if (!OwnerActor->IsUsingRegisteredSubObjectList())
	{
		UE_LOG(LogObjectPool, Warning,
			TEXT("Cannot register pooled subobject %s: owner %s does not use the registered subobject list."),
			*GetNameSafe(Object), *GetNameSafe(OwnerActor));
		return;
	}

	FObjectPoolReplicatedOwnerBinding Binding;
	Binding.Actor = OwnerActor;
	Binding.Component = OwnerComponent;
	if (OwnerComponent != nullptr)
	{
		if (!OwnerComponent->GetIsReplicated())
		{
			UE_LOG(LogObjectPool, Warning,
				TEXT("Cannot register pooled subobject %s: owner component %s is not replicated."),
				*GetNameSafe(Object), *GetNameSafe(OwnerComponent));
			return;
		}
		OwnerComponent->AddReplicatedSubObject(Object, COND_None);
	}
	else
	{
		OwnerActor->AddReplicatedSubObject(Object, COND_None);
	}

	ReplicatedObjectOwners.Add(Object, Binding);
	OwnerActor->ForceNetUpdate();
}

void UObjectPoolSubsystem::UnregisterObjectFromReplication(
	UObject* Object,
	const EObjectPoolRemoteSubObjectReleasePolicy ReleasePolicy)
{
	FObjectPoolReplicatedOwnerBinding Binding;
	if (!ReplicatedObjectOwners.RemoveAndCopyValue(Object, Binding) || !IsValid(Binding.Actor))
	{
		return;
	}

	const bool bComponentOwned = IsValid(Binding.Component);
	switch (ReleasePolicy)
	{
	case EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica:
		if (bComponentOwned)
		{
			Binding.Actor->DestroyReplicatedSubObjectOnRemotePeers(Binding.Component, Object);
		}
		else
		{
			Binding.Actor->DestroyReplicatedSubObjectOnRemotePeers(Object);
		}
		break;

	case EObjectPoolRemoteSubObjectReleasePolicy::TearOffRemoteReplica:
		if (bComponentOwned)
		{
			Binding.Actor->TearOffReplicatedSubObjectOnRemotePeers(Binding.Component, Object);
		}
		else
		{
			Binding.Actor->TearOffReplicatedSubObjectOnRemotePeers(Object);
		}
		break;

	case EObjectPoolRemoteSubObjectReleasePolicy::UnregisterOnly:
	default:
		if (bComponentOwned)
		{
			Binding.Component->RemoveReplicatedSubObject(Object);
		}
		else
		{
			Binding.Actor->RemoveReplicatedSubObject(Object);
		}
		break;
	}

	Binding.Actor->ForceNetUpdate();
}

void UObjectPoolSubsystem::ActivateComponentReplication(
	UActorComponent* Component,
	const FComponentPoolClassConfig& Config)
{
	AActor* Owner = IsValid(Component) ? Component->GetOwner() : nullptr;
	if (!Config.bReplicateComponent || !IsObjectPoolNetworkSupportEnabled() || !IsValid(Owner) ||
		!Owner->HasAuthority() || !Owner->GetIsReplicated() || !Component->GetComponentClassCanReplicate())
	{
		return;
	}

	Component->SetIsReplicated(true);
	Owner->ForceNetUpdate();
}

void UObjectPoolSubsystem::DeactivateComponentReplication(
	UActorComponent* Component,
	const EObjectPoolRemoteSubObjectReleasePolicy ReleasePolicy)
{
	AActor* Owner = IsValid(Component) ? Component->GetOwner() : nullptr;
	if (IsValid(Owner) && Owner->HasAuthority() && Component->GetIsReplicated())
	{
		switch (ReleasePolicy)
		{
		case EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica:
			Owner->DestroyReplicatedSubObjectOnRemotePeers(Component);
			break;
		case EObjectPoolRemoteSubObjectReleasePolicy::TearOffRemoteReplica:
			Owner->TearOffReplicatedSubObjectOnRemotePeers(Component);
			break;
		case EObjectPoolRemoteSubObjectReleasePolicy::UnregisterOnly:
		default:
			break;
		}
		Component->SetIsReplicated(false);
		Owner->ForceNetUpdate();
	}
}

void UObjectPoolSubsystem::InvokeAcquireCallback(AActor* Actor, const FActorPoolAcquireContext& Context) const
{
	if (!Actor->GetClass()->ImplementsInterface(UPoolableActorInterface::StaticClass()))
	{
		return;
	}

	if (GetWorld()->GetNetMode() == NM_Client)
	{
		ObjectPoolActorCallbacks::AcquireClient(Actor, Context);
	}
	else
	{
		ObjectPoolActorCallbacks::AcquireServer(Actor, Context);
	}
}

void UObjectPoolSubsystem::InvokeReleaseCallback(AActor* Actor) const
{
	if (!Actor->GetClass()->ImplementsInterface(UPoolableActorInterface::StaticClass()))
	{
		return;
	}

	if (GetWorld()->GetNetMode() == NM_Client)
	{
		ObjectPoolActorCallbacks::ReleaseClient(Actor);
	}
	else
	{
		ObjectPoolActorCallbacks::ReleaseServer(Actor);
	}
}

void UObjectPoolSubsystem::RemoveInvalidManagedActors()
{
	for (auto It = ManagedActors.CreateIterator(); It; ++It)
	{
		if (!IsValid(It.Key()))
		{
			if (FPooledObjectBucket* Bucket = ActorPools.Find(It.Value()))
			{
				Bucket->Remove(It.Key());
			}
			ActorTransitions.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
}

void UObjectPoolSubsystem::RemoveInvalidManagedObjects()
{
	for (auto It = ManagedObjects.CreateIterator(); It; ++It)
	{
		if (!IsValid(It.Key()))
		{
			if (FPooledObjectBucket* Bucket = ObjectPools.Find(It.Value()))
			{
				Bucket->Remove(It.Key());
			}
			ObjectTransitions.Remove(It.Key());
			ReplicatedObjectOwners.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
}

void UObjectPoolSubsystem::RemoveInvalidManagedComponents()
{
	for (auto It = ManagedComponents.CreateIterator(); It; ++It)
	{
		if (!IsValid(It.Key()))
		{
			if (FPooledObjectBucket* Bucket = ComponentPools.Find(It.Value()))
			{
				Bucket->Remove(It.Key());
			}
			ComponentTransitions.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
}

void UObjectPoolSubsystem::BeginActorPrewarm()
{
	bActorPrewarmComplete = false;
	ActorPrewarmHandle.Reset();

	const UObjectPoolSettings* Settings = GetDefault<UObjectPoolSettings>();
	if (!Settings->bEnabled || !IsObjectPoolRuntimeEnabled())
	{
		bActorPrewarmComplete = true;
		return;
	}

	const FPoolModeConfig* ModeConfig = FindCurrentModeConfig();

	if (ModeConfig == nullptr)
	{
		bActorPrewarmComplete = true;
		return;
	}

	TSet<FSoftObjectPath> UniqueClassPaths;
	for (const FActorPoolClassConfig& Config : ModeConfig->ActorPools)
	{
		if (Config.bEnabled && GetPreallocateCount(Config) > 0 && !Config.ActorClass.IsNull())
		{
			UniqueClassPaths.Add(Config.ActorClass.ToSoftObjectPath());
		}
	}

	if (UniqueClassPaths.IsEmpty())
	{
		bActorPrewarmComplete = true;
		return;
	}

	TArray<FSoftObjectPath> ClassPaths = UniqueClassPaths.Array();
	TWeakObjectPtr<UObjectPoolSubsystem> WeakThis(this);
	ActorPrewarmHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		ClassPaths,
		FStreamableDelegate::CreateLambda([WeakThis]()
		{
			if (UObjectPoolSubsystem* Subsystem = WeakThis.Get())
			{
				Subsystem->CompleteActorPrewarm();
			}
		}),
		FStreamableManager::AsyncLoadHighPriority,
		false,
		false,
		TEXT("ObjectPoolActorPrewarm"));

	if (!ActorPrewarmHandle.IsValid())
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Unable to start actor pool async prewarm."));
		bActorPrewarmComplete = true;
	}
}

void UObjectPoolSubsystem::BeginObjectPrewarm()
{
	bObjectPrewarmComplete = false;
	ObjectPrewarmHandle.Reset();

	const UObjectPoolSettings* Settings = GetDefault<UObjectPoolSettings>();
	if (!Settings->bEnabled || !IsObjectPoolRuntimeEnabled())
	{
		bObjectPrewarmComplete = true;
		return;
	}

	const FPoolModeConfig* ModeConfig = FindCurrentModeConfig();
	if (ModeConfig == nullptr)
	{
		bObjectPrewarmComplete = true;
		return;
	}

	TSet<FSoftObjectPath> UniqueClassPaths;
	for (const FObjectPoolClassConfig& Config : ModeConfig->ObjectPools)
	{
		if (Config.bEnabled && GetPreallocateCount(Config) > 0 && !Config.ObjectClass.IsNull())
		{
			UniqueClassPaths.Add(Config.ObjectClass);
		}
	}

	if (UniqueClassPaths.IsEmpty())
	{
		bObjectPrewarmComplete = true;
		return;
	}

	TWeakObjectPtr<UObjectPoolSubsystem> WeakThis(this);
	ObjectPrewarmHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		UniqueClassPaths.Array(),
		FStreamableDelegate::CreateLambda([WeakThis]()
		{
			if (UObjectPoolSubsystem* Subsystem = WeakThis.Get())
			{
				Subsystem->CompleteObjectPrewarm();
			}
		}),
		FStreamableManager::AsyncLoadHighPriority,
		false,
		false,
		TEXT("ObjectPoolUObjectPrewarm"));

	if (!ObjectPrewarmHandle.IsValid())
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Unable to start UObject pool async prewarm."));
		bObjectPrewarmComplete = true;
	}
}

void UObjectPoolSubsystem::BeginComponentPrewarm()
{
	bComponentPrewarmComplete = false;
	ComponentPrewarmHandle.Reset();

	const UObjectPoolSettings* Settings = GetDefault<UObjectPoolSettings>();
	const FPoolModeConfig* ModeConfig = FindCurrentModeConfig();
	if (!Settings->bEnabled || !IsObjectPoolRuntimeEnabled() || ModeConfig == nullptr)
	{
		bComponentPrewarmComplete = true;
		return;
	}

	TSet<FSoftObjectPath> UniqueClassPaths;
	for (const FComponentPoolClassConfig& Config : ModeConfig->ComponentPools)
	{
		if (Config.bEnabled && GetPreallocateCount(Config) > 0 && !Config.ComponentClass.IsNull())
		{
			UniqueClassPaths.Add(Config.ComponentClass);
		}
	}

	if (UniqueClassPaths.IsEmpty())
	{
		bComponentPrewarmComplete = true;
		return;
	}

	TWeakObjectPtr<UObjectPoolSubsystem> WeakThis(this);
	ComponentPrewarmHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		UniqueClassPaths.Array(),
		FStreamableDelegate::CreateLambda([WeakThis]()
		{
			if (UObjectPoolSubsystem* Subsystem = WeakThis.Get())
			{
				Subsystem->CompleteComponentPrewarm();
			}
		}),
		FStreamableManager::AsyncLoadHighPriority,
		false,
		false,
		TEXT("ObjectPoolComponentPrewarm"));

	if (!ComponentPrewarmHandle.IsValid())
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Unable to start component pool async prewarm."));
		bComponentPrewarmComplete = true;
	}
}

void UObjectPoolSubsystem::CompleteActorPrewarm()
{
	if (bIsDeinitializing || GetWorld() == nullptr)
	{
		return;
	}

	const FPoolModeConfig* ModeConfig = FindCurrentModeConfig();

	if (ModeConfig != nullptr)
	{
		TSet<UClass*> ProcessedClasses;
		for (const FActorPoolClassConfig& Config : ModeConfig->ActorPools)
		{
			UClass* Class = Config.ActorClass.Get();
			if (!Config.bEnabled || Class == nullptr || ProcessedClasses.Contains(Class))
			{
				continue;
			}

			ProcessedClasses.Add(Class);
			PreallocateActorClass(Config);
		}
	}

	bActorPrewarmComplete = true;
	ActorPrewarmHandle.Reset();
	UE_LOG(LogObjectPool, Log, TEXT("Actor pool prewarm completed for world %s using mode %s."),
		*GetNameSafe(GetWorld()), *CurrentModeTag.ToString());
}

void UObjectPoolSubsystem::CompleteObjectPrewarm()
{
	if (bIsDeinitializing || GetWorld() == nullptr)
	{
		return;
	}

	if (const FPoolModeConfig* ModeConfig = FindCurrentModeConfig())
	{
		TSet<UClass*> ProcessedClasses;
		for (const FObjectPoolClassConfig& Config : ModeConfig->ObjectPools)
		{
			UClass* Class = Config.ObjectClass.ResolveClass();
			if (!Config.bEnabled || Class == nullptr || ProcessedClasses.Contains(Class))
			{
				continue;
			}
			ProcessedClasses.Add(Class);
			PreallocateObjectClass(Config);
		}
	}

	bObjectPrewarmComplete = true;
	ObjectPrewarmHandle.Reset();
	UE_LOG(LogObjectPool, Log, TEXT("UObject pool prewarm completed for world %s using mode %s."),
		*GetNameSafe(GetWorld()), *CurrentModeTag.ToString());
}

void UObjectPoolSubsystem::CompleteComponentPrewarm()
{
	if (bIsDeinitializing || GetWorld() == nullptr)
	{
		return;
	}

	if (const FPoolModeConfig* ModeConfig = FindCurrentModeConfig())
	{
		TSet<UClass*> ProcessedClasses;
		for (const FComponentPoolClassConfig& Config : ModeConfig->ComponentPools)
		{
			UClass* Class = Config.ComponentClass.ResolveClass();
			if (!Config.bEnabled || Class == nullptr || ProcessedClasses.Contains(Class))
			{
				continue;
			}
			ProcessedClasses.Add(Class);
			PreallocateComponentClass(Config);
		}
	}

	bComponentPrewarmComplete = true;
	ComponentPrewarmHandle.Reset();
	UE_LOG(LogObjectPool, Log, TEXT("Component pool prewarm completed for world %s using mode %s."),
		*GetNameSafe(GetWorld()), *CurrentModeTag.ToString());
}

void UObjectPoolSubsystem::PreallocateActorClass(const FActorPoolClassConfig& Config)
{
	UClass* Class = Config.ActorClass.Get();
	if (Class == nullptr || !Class->IsChildOf(AActor::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Skipping invalid actor prewarm class %s."), *GetNameSafe(Class));
		return;
	}

	FPooledObjectBucket& Bucket = FindOrAddActorBucket(Class, Config);
	const int32 DesiredInactiveCount = FMath::Clamp(GetPreallocateCount(Config), 0, Config.MaxPoolSize);
	while (!bIsDeinitializing && Bucket.NumInactive() < DesiredInactiveCount && Bucket.NumTotal() < Config.MaxPoolSize)
	{
		AActor* Actor = CreateActor(Class, FTransform::Identity, nullptr, nullptr);
		if (Actor == nullptr)
		{
			break;
		}

		if (Bucket.RegisterActive(Actor) != EObjectPoolOperationResult::Succeeded)
		{
			Actor->Destroy();
			break;
		}

		ManagedActors.Add(Actor, Class);
		ActorTransitions.Add(Actor, EObjectPoolEntryState::Preallocating);
		FObjectPoolClassStats& Stats = ActorStats.FindOrAdd(Class);
		++Stats.Created;
		++Stats.Preallocated;

		if (Actor->GetClass()->ImplementsInterface(UPoolableActorInterface::StaticClass()))
		{
			ObjectPoolActorCallbacks::Created(Actor);
		}
		if (!IsValid(Actor))
		{
			Bucket.Remove(Actor);
			ManagedActors.Remove(Actor);
			ActorTransitions.Remove(Actor);
			break;
		}

		InvokeReleaseCallback(Actor);
		if (!IsValid(Actor))
		{
			Bucket.Remove(Actor);
			ManagedActors.Remove(Actor);
			ActorTransitions.Remove(Actor);
			break;
		}
		DeactivateActor(Actor, Config.RecoveryPolicy);
		if (Bucket.Release(Actor) != EObjectPoolOperationResult::Succeeded)
		{
			Bucket.Remove(Actor);
			ManagedActors.Remove(Actor);
			ActorTransitions.Remove(Actor);
			Actor->Destroy();
			break;
		}
		ActorTransitions.Remove(Actor);
	}
}

void UObjectPoolSubsystem::PreallocateObjectClass(const FObjectPoolClassConfig& Config)
{
	UClass* Class = Config.ObjectClass.ResolveClass();
	if (Class == nullptr || Class->IsChildOf(AActor::StaticClass()) || Class->IsChildOf(UActorComponent::StaticClass()) ||
		Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Skipping invalid UObject prewarm class %s."), *GetNameSafe(Class));
		return;
	}

	FPooledObjectBucket& Bucket = FindOrAddObjectBucket(Class, Config);
	const int32 DesiredInactiveCount = FMath::Clamp(GetPreallocateCount(Config), 0, Config.MaxPoolSize);
	while (!bIsDeinitializing && Bucket.NumInactive() < DesiredInactiveCount && Bucket.NumTotal() < Config.MaxPoolSize)
	{
		UObject* Object = CreateObject(Class);
		if (Object == nullptr || Bucket.RegisterActive(Object) != EObjectPoolOperationResult::Succeeded)
		{
			break;
		}

		ManagedObjects.Add(Object, Class);
		ObjectTransitions.Add(Object, EObjectPoolEntryState::Preallocating);
		FObjectPoolClassStats& Stats = ObjectStats.FindOrAdd(Class);
		++Stats.Created;
		++Stats.Preallocated;
		InvokeObjectCreatedCallback(Object);
		InvokeObjectReleaseCallback(Object);
		if (!IsValid(Object))
		{
			Bucket.Remove(Object);
			ManagedObjects.Remove(Object);
			ObjectTransitions.Remove(Object);
			break;
		}
		DeactivateObject(Object, Config.RecoveryPolicy);

		if (Bucket.Release(Object) != EObjectPoolOperationResult::Succeeded)
		{
			Bucket.Remove(Object);
			ManagedObjects.Remove(Object);
			ObjectTransitions.Remove(Object);
			Object->MarkAsGarbage();
			break;
		}
		ObjectTransitions.Remove(Object);
	}
}

void UObjectPoolSubsystem::PreallocateComponentClass(const FComponentPoolClassConfig& Config)
{
	UClass* Class = Config.ComponentClass.ResolveClass();
	if (Class == nullptr || !Class->IsChildOf(UActorComponent::StaticClass()) ||
		Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated) || EnsureComponentPoolOwner() == nullptr)
	{
		UE_LOG(LogObjectPool, Warning, TEXT("Skipping invalid component prewarm class %s."), *GetNameSafe(Class));
		return;
	}

	FPooledObjectBucket& Bucket = FindOrAddComponentBucket(Class, Config);
	const int32 DesiredInactiveCount = FMath::Clamp(GetPreallocateCount(Config), 0, Config.MaxPoolSize);
	while (!bIsDeinitializing && Bucket.NumInactive() < DesiredInactiveCount && Bucket.NumTotal() < Config.MaxPoolSize)
	{
		UActorComponent* Component = CreateComponent(Class);
		if (Component == nullptr || Bucket.RegisterActive(Component) != EObjectPoolOperationResult::Succeeded)
		{
			break;
		}

		ManagedComponents.Add(Component, Class);
		ComponentTransitions.Add(Component, EObjectPoolEntryState::Preallocating);
		FObjectPoolClassStats& Stats = ComponentStats.FindOrAdd(Class);
		++Stats.Created;
		++Stats.Preallocated;
		Component->RegisterComponentWithWorld(GetWorld());
		InvokeComponentCreatedCallback(Component);
		InvokeComponentReleaseCallback(Component);
		if (!IsValid(Component))
		{
			Bucket.Remove(Component);
			ManagedComponents.Remove(Component);
			ComponentTransitions.Remove(Component);
			break;
		}
		DeactivateComponent(Component, Config.RecoveryPolicy);

		if (Bucket.Release(Component) != EObjectPoolOperationResult::Succeeded)
		{
			Bucket.Remove(Component);
			ManagedComponents.Remove(Component);
			ComponentTransitions.Remove(Component);
			Component->MarkAsGarbage();
			break;
		}
		ComponentTransitions.Remove(Component);
	}
}

int32 UObjectPoolSubsystem::GetPreallocateCount(const FPoolClassConfigBase& Config) const
{
	const UWorld* World = GetWorld();
#if WITH_EDITOR
	if (World != nullptr && World->WorldType == EWorldType::PIE)
	{
		return Config.EditorPreallocateCount;
	}
#endif

	if (World == nullptr)
	{
		return 0;
	}

	switch (World->GetNetMode())
	{
	case NM_DedicatedServer:
	case NM_ListenServer:
		return Config.ServerPreallocateCount;
	case NM_Client:
	case NM_Standalone:
	default:
		return Config.ClientPreallocateCount;
	}
}

void UObjectPoolSubsystem::ValidateAfterMutation(const TCHAR* Operation) const
{
	if (!IsObjectPoolMutationValidationEnabled())
	{
		return;
	}

	FString Error;
	if (!Validate(&Error))
	{
		UE_LOG(LogObjectPool, Error, TEXT("Pool invariant failure after %s in world %s: %s"),
			Operation, *GetNameSafe(GetWorld()), *Error);
	}
}
