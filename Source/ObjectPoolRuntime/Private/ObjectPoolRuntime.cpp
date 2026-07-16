#include "ObjectPoolRuntime.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "ObjectPoolNetworkSmokeActor.h"
#include "ObjectPoolSettings.h"
#include "ObjectPoolSubsystem.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY(LogObjectPool);

namespace ObjectPoolConsole
{
	TAutoConsoleVariable<int32> Enabled(
		TEXT("Pool.Enabled"),
		1,
		TEXT("Enables object-pool acquisition and prewarming. 0=disabled, 1=enabled."),
		ECVF_Default);

	TAutoConsoleVariable<int32> DebugLogging(
		TEXT("Pool.Debug"),
		0,
		TEXT("Logs actor pool hits, misses, releases, and fallback reasons."),
		ECVF_Default);

	TAutoConsoleVariable<int32> ValidateMutations(
		TEXT("Pool.ValidateOnMutation"),
		0,
		TEXT("Validates all pool invariants after each acquire or release."),
		ECVF_Default);

	TAutoConsoleVariable<int32> NetworkEnabled(
		TEXT("Pool.Network.Enabled"),
		1,
		TEXT("Enables public-API actor pool replication state and dormancy handling."),
		ECVF_Default);

	void ForEachPoolSubsystem(TFunctionRef<void(UObjectPoolSubsystem&)> Callback)
	{
		if (GEngine == nullptr)
		{
			return;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr)
			{
				continue;
			}

			if (UObjectPoolSubsystem* Subsystem = World->GetSubsystem<UObjectPoolSubsystem>())
			{
				Callback(*Subsystem);
			}
		}
	}

	FAutoConsoleCommand DumpCommand(
		TEXT("Pool.Dump"),
		TEXT("Prints object-pool state for every supported world."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			ForEachPoolSubsystem([](UObjectPoolSubsystem& Subsystem)
			{
				UE_LOG(LogObjectPool, Display, TEXT("%s"), *Subsystem.BuildDebugReport());
			});
		}));

	FAutoConsoleCommand ValidateCommand(
		TEXT("Pool.Validate"),
		TEXT("Validates object-pool invariants for every supported world."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			ForEachPoolSubsystem([](UObjectPoolSubsystem& Subsystem)
			{
				FString Error;
				if (Subsystem.Validate(&Error))
				{
					UE_LOG(LogObjectPool, Display, TEXT("Pool validation succeeded for %s."), *GetNameSafe(Subsystem.GetWorld()));
				}
				else
				{
					UE_LOG(LogObjectPool, Error, TEXT("Pool validation failed for %s: %s"), *GetNameSafe(Subsystem.GetWorld()), *Error);
				}
			});
		}));

#if !UE_BUILD_SHIPPING
	TAutoConsoleVariable<float> NetSmokeStartDelay(
		TEXT("Pool.NetSmoke.StartDelay"),
		15.0f,
		TEXT("Seconds Pool.NetSmoke waits before its first acquire; later phases run at +3 second intervals."),
		ECVF_Default);

	TAutoConsoleVariable<int32> NetSmokeReleasePolicy(
		TEXT("Pool.NetSmoke.ReleasePolicy"),
		1,
		TEXT("Remote pooled subobject release policy used by Pool.NetSmoke: 0=UnregisterOnly, 1=DestroyRemoteReplica, 2=TearOffRemoteReplica."),
		ECVF_Default);

	EObjectPoolRemoteSubObjectReleasePolicy ResolveNetSmokeReleasePolicy()
	{
		switch (NetSmokeReleasePolicy.GetValueOnGameThread())
		{
		case 0:
			return EObjectPoolRemoteSubObjectReleasePolicy::UnregisterOnly;
		case 2:
			return EObjectPoolRemoteSubObjectReleasePolicy::TearOffRemoteReplica;
		case 1:
		default:
			return EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica;
		}
	}

	struct FNetSmokeState
	{
		TWeakObjectPtr<UObjectPoolSubsystem> Subsystem;
		TWeakObjectPtr<AActor> FirstActor;
		TWeakObjectPtr<AActor> OwnerA;
		TWeakObjectPtr<AActor> OwnerB;
		TWeakObjectPtr<UObjectPoolNetworkSmokeObject> FirstObject;
		TWeakObjectPtr<UObjectPoolNetworkSmokeComponent> FirstComponent;
		EObjectPoolRemoteSubObjectReleasePolicy ReleasePolicy = EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica;
		bool bFailed = false;
	};

	FAutoConsoleCommand NetSmokeCommand(
		TEXT("Pool.NetSmoke"),
		TEXT("Runs replicated Actor reuse plus UObject/Component cross-owner reuse in authority game worlds."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			ForEachPoolSubsystem([](UObjectPoolSubsystem& Subsystem)
			{
				UWorld* World = Subsystem.GetWorld();
				if (World == nullptr || World->GetNetMode() == NM_Client)
				{
					return;
				}

				UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
				FActorPoolClassConfig* ActorConfig = Settings->DefaultModeConfig.ActorPools.FindByPredicate([](const FActorPoolClassConfig& Config)
				{
					return Config.ActorClass.Get() == AObjectPoolNetworkSmokeActor::StaticClass();
				});
				if (ActorConfig == nullptr)
				{
					FActorPoolClassConfig Config;
					Config.ActorClass = AObjectPoolNetworkSmokeActor::StaticClass();
					Config.MaxPoolSize = 3;
					Config.bAllowRuntimeGrowth = true;
					Config.RecoveryPolicy = EObjectPoolRecoveryPolicy::ResetFrameworkState;
					Settings->DefaultModeConfig.ActorPools.Add(Config);
					ActorConfig = &Settings->DefaultModeConfig.ActorPools.Last();
				}
				ActorConfig->MaxPoolSize = FMath::Max(ActorConfig->MaxPoolSize, 3);

				const EObjectPoolRemoteSubObjectReleasePolicy ReleasePolicy = ResolveNetSmokeReleasePolicy();
				FObjectPoolClassConfig* ObjectConfig = Settings->DefaultModeConfig.ObjectPools.FindByPredicate([](const FObjectPoolClassConfig& Config)
				{
					return Config.ObjectClass.ResolveClass() == UObjectPoolNetworkSmokeObject::StaticClass();
				});
				if (ObjectConfig == nullptr)
				{
					FObjectPoolClassConfig Config;
					Config.ObjectClass = FSoftClassPath(UObjectPoolNetworkSmokeObject::StaticClass());
					Config.MaxPoolSize = 1;
					Config.bAllowRuntimeGrowth = true;
					Config.bRegisterAsReplicatedSubObject = true;
					Config.RemoteReleasePolicy = ReleasePolicy;
					Settings->DefaultModeConfig.ObjectPools.Add(Config);
					ObjectConfig = &Settings->DefaultModeConfig.ObjectPools.Last();
				}
				ObjectConfig->RemoteReleasePolicy = ReleasePolicy;

				FComponentPoolClassConfig* ComponentConfig = Settings->DefaultModeConfig.ComponentPools.FindByPredicate([](const FComponentPoolClassConfig& Config)
				{
					return Config.ComponentClass.ResolveClass() == UObjectPoolNetworkSmokeComponent::StaticClass();
				});
				if (ComponentConfig == nullptr)
				{
					FComponentPoolClassConfig Config;
					Config.ComponentClass = FSoftClassPath(UObjectPoolNetworkSmokeComponent::StaticClass());
					Config.MaxPoolSize = 1;
					Config.bAllowRuntimeGrowth = true;
					Config.bReplicateComponent = true;
					Config.RemoteReleasePolicy = ReleasePolicy;
					Settings->DefaultModeConfig.ComponentPools.Add(Config);
					ComponentConfig = &Settings->DefaultModeConfig.ComponentPools.Last();
				}
				ComponentConfig->RemoteReleasePolicy = ReleasePolicy;

				TSharedRef<FNetSmokeState> State = MakeShared<FNetSmokeState>();
				State->Subsystem = &Subsystem;
				State->ReleasePolicy = ReleasePolicy;
				const float StartDelay = FMath::Max(1.0f, NetSmokeStartDelay.GetValueOnGameThread());
				UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_SMOKE_SCHEDULED world=%s netmode=%d delay=%.1f policy=%d"),
					*World->GetName(), static_cast<int32>(World->GetNetMode()), StartDelay, static_cast<int32>(ReleasePolicy));

				FTimerManager& Timers = World->GetTimerManager();
				FTimerHandle FirstAcquireHandle;
				Timers.SetTimer(FirstAcquireHandle, FTimerDelegate::CreateLambda([State]()
				{
					if (UObjectPoolSubsystem* Pool = State->Subsystem.Get())
					{
						AActor* Actor = Pool->SpawnActorFromPool(AObjectPoolNetworkSmokeActor::StaticClass(), FTransform::Identity);
						AActor* OwnerA = Pool->SpawnActorFromPool(
							AObjectPoolNetworkSmokeActor::StaticClass(), FTransform(FVector(300.0, 0.0, 0.0)));
						AActor* OwnerB = Pool->SpawnActorFromPool(
							AObjectPoolNetworkSmokeActor::StaticClass(), FTransform(FVector(600.0, 0.0, 0.0)));
						State->FirstActor = Actor;
						State->OwnerA = OwnerA;
						State->OwnerB = OwnerB;
						UObjectPoolNetworkSmokeObject* Object = OwnerA != nullptr
							? Cast<UObjectPoolNetworkSmokeObject>(Pool->GetObjectFromPool(OwnerA, UObjectPoolNetworkSmokeObject::StaticClass()))
							: nullptr;
						UObjectPoolNetworkSmokeComponent* Component = OwnerA != nullptr
							? Cast<UObjectPoolNetworkSmokeComponent>(Pool->GetComponentFromPool(OwnerA, UObjectPoolNetworkSmokeComponent::StaticClass()))
							: nullptr;
						State->FirstObject = Object;
						State->FirstComponent = Component;
						State->bFailed |= Actor == nullptr || OwnerA == nullptr || OwnerB == nullptr || Object == nullptr || Component == nullptr;
						for (AActor* ReplicatedActor : { Actor, OwnerA, OwnerB })
						{
							if (ReplicatedActor != nullptr)
							{
								ReplicatedActor->ForceNetUpdate();
							}
						}
						UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_SMOKE_FIRST_ACQUIRE actor=%s owner_a=%s owner_b=%s"),
							*GetNameSafe(Actor), *GetNameSafe(OwnerA), *GetNameSafe(OwnerB));
					}
				}), StartDelay, false);

				FTimerHandle FirstReleaseHandle;
				Timers.SetTimer(FirstReleaseHandle, FTimerDelegate::CreateLambda([State]()
				{
					if (UObjectPoolSubsystem* Pool = State->Subsystem.Get())
					{
						const bool bObjectReleased = Pool->ReleaseObjectToPool(State->FirstObject.Get());
						const bool bComponentReleased = Pool->ReleaseComponentToPool(State->FirstComponent.Get());
						const bool bActorReleased = Pool->ReleaseActorToPool(State->FirstActor.Get());
						State->bFailed |= !bObjectReleased || !bComponentReleased || !bActorReleased;
						UE_LOG(LogObjectPool, Display,
							TEXT("POOL_NET_SMOKE_FIRST_RELEASE actor=%s object=%s component=%s"),
							bActorReleased ? TEXT("true") : TEXT("false"),
							bObjectReleased ? TEXT("true") : TEXT("false"),
							bComponentReleased ? TEXT("true") : TEXT("false"));
					}
				}), StartDelay + 3.0f, false);

				FTimerHandle ReacquireHandle;
				Timers.SetTimer(ReacquireHandle, FTimerDelegate::CreateLambda([State]()
				{
					if (UObjectPoolSubsystem* Pool = State->Subsystem.Get())
					{
						AActor* Reused = Pool->SpawnActorFromPool(AObjectPoolNetworkSmokeActor::StaticClass(), FTransform(FVector(100.0, 0.0, 0.0)));
						const bool bReusedSameActor = Reused != nullptr && Reused == State->FirstActor.Get();
						AActor* OwnerB = State->OwnerB.Get();
						UObject* ReusedObject = OwnerB != nullptr
							? Pool->GetObjectFromPool(OwnerB, UObjectPoolNetworkSmokeObject::StaticClass())
							: nullptr;
						UActorComponent* ReusedComponent = OwnerB != nullptr
							? Pool->GetComponentFromPool(OwnerB, UObjectPoolNetworkSmokeComponent::StaticClass())
							: nullptr;
						const bool bReusedSameObject = ReusedObject != nullptr && ReusedObject == State->FirstObject.Get();
						const bool bReusedSameComponent = ReusedComponent != nullptr && ReusedComponent == State->FirstComponent.Get();
						const bool bExpectSubObjectReuse =
							State->ReleasePolicy == EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica;
						State->bFailed |= !bReusedSameActor || ReusedObject == nullptr || ReusedComponent == nullptr ||
							bReusedSameObject != bExpectSubObjectReuse || bReusedSameComponent != bExpectSubObjectReuse;
						State->FirstObject = Cast<UObjectPoolNetworkSmokeObject>(ReusedObject);
						State->FirstComponent = Cast<UObjectPoolNetworkSmokeComponent>(ReusedComponent);
						if (Reused != nullptr)
						{
							Reused->ForceNetUpdate();
						}
						if (OwnerB != nullptr)
						{
							OwnerB->ForceNetUpdate();
						}
						UE_LOG(LogObjectPool, Display,
							TEXT("POOL_NET_SMOKE_REACQUIRE actor=%s owner_b=%s actor_reused=%s object_reused=%s component_reused=%s expected_subobject_reuse=%s"),
							*GetNameSafe(Reused),
							*GetNameSafe(OwnerB),
							bReusedSameActor ? TEXT("true") : TEXT("false"),
							bReusedSameObject ? TEXT("true") : TEXT("false"),
							bReusedSameComponent ? TEXT("true") : TEXT("false"),
							bExpectSubObjectReuse ? TEXT("true") : TEXT("false"));
					}
				}), StartDelay + 6.0f, false);

				FTimerHandle FinalReleaseHandle;
				Timers.SetTimer(FinalReleaseHandle, FTimerDelegate::CreateLambda([State]()
				{
					if (UObjectPoolSubsystem* Pool = State->Subsystem.Get())
					{
						const bool bObjectReleased = Pool->ReleaseObjectToPool(State->FirstObject.Get());
						const bool bComponentReleased = Pool->ReleaseComponentToPool(State->FirstComponent.Get());
						const bool bReleased = Pool->ReleaseActorToPool(State->FirstActor.Get());
						const bool bOwnerAReleased = Pool->ReleaseActorToPool(State->OwnerA.Get());
						const bool bOwnerBReleased = Pool->ReleaseActorToPool(State->OwnerB.Get());
						FString Error;
						const bool bValid = Pool->Validate(&Error);
						State->bFailed |= !bReleased || !bOwnerAReleased || !bOwnerBReleased ||
							!bObjectReleased || !bComponentReleased || !bValid;
						if (State->bFailed)
						{
							UE_LOG(LogObjectPool, Error, TEXT("POOL_NET_SMOKE_FAIL world=%s validation=%s error=%s"),
								*GetNameSafe(Pool->GetWorld()), bValid ? TEXT("true") : TEXT("false"), *Error);
						}
						else
						{
							UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_SMOKE_PASS world=%s validation=true policy=%d"),
								*GetNameSafe(Pool->GetWorld()), static_cast<int32>(State->ReleasePolicy));
						}
					}
				}), StartDelay + 9.0f, false);
			});
		}));
#endif
}

bool IsObjectPoolRuntimeEnabled()
{
	return ObjectPoolConsole::Enabled.GetValueOnGameThread() != 0;
}

bool IsObjectPoolDebugLoggingEnabled()
{
	return ObjectPoolConsole::DebugLogging.GetValueOnGameThread() != 0;
}

bool IsObjectPoolMutationValidationEnabled()
{
	return ObjectPoolConsole::ValidateMutations.GetValueOnGameThread() != 0;
}

bool IsObjectPoolNetworkSupportEnabled()
{
	return ObjectPoolConsole::NetworkEnabled.GetValueOnGameThread() != 0;
}

IMPLEMENT_MODULE(FDefaultModuleImpl, ObjectPoolRuntime);
