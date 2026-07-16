#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "ObjectPoolTypes.generated.h"

UENUM(BlueprintType)
enum class EObjectPoolEntryState : uint8
{
	Unmanaged,
	Preallocating,
	Active,
	Releasing,
	Inactive
};

UENUM(BlueprintType)
enum class EObjectPoolRecoveryPolicy : uint8
{
	/** Only invokes pool lifecycle callbacks. Mandatory ownership-transfer cleanup may still run. */
	BusinessCallbacksOnly UMETA(DisplayName = "Business Callbacks Only"),
	/** Resets framework-owned runtime state such as timers, ticking, activation and registration. */
	ResetFrameworkState UMETA(DisplayName = "Reset Framework State"),
	/**
	 * Performs the strongest framework-owned cleanup, including latent actions and attachments.
	 * It cannot reset arbitrary gameplay variables, delegates, effects, materials, abilities or references;
	 * those remain the responsibility of the pool lifecycle callbacks.
	 */
	Full UMETA(DisplayName = "Full Framework Reset (Business Reset Required)")
};

UENUM(BlueprintType)
enum class EObjectPoolOperationResult : uint8
{
	Succeeded,
	InvalidObject,
	AlreadyActive,
	AlreadyInactive,
	NotActive,
	CapacityReached
};

UENUM(BlueprintType)
enum class EObjectPoolRemoteSubObjectReleasePolicy : uint8
{
	/** Stops future replication without explicitly removing an existing remote replica. */
	UnregisterOnly,
	/** Removes the current remote replica before the server instance can be reused. */
	DestroyRemoteReplica,
	/** Leaves a torn-off remote replica that is no longer controlled by the server. */
	TearOffRemoteReplica
};

USTRUCT(BlueprintType)
struct OBJECTPOOLRUNTIME_API FPoolClassConfigBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool", meta = (ClampMin = "0"))
	int32 EditorPreallocateCount = 0;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool", meta = (ClampMin = "0"))
	int32 ServerPreallocateCount = 0;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool", meta = (ClampMin = "0"))
	int32 ClientPreallocateCount = 0;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool", meta = (ClampMin = "1"))
	int32 MaxPoolSize = 64;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool")
	bool bAllowRuntimeGrowth = true;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool")
	EObjectPoolRecoveryPolicy RecoveryPolicy = EObjectPoolRecoveryPolicy::ResetFrameworkState;
};

USTRUCT(BlueprintType)
struct OBJECTPOOLRUNTIME_API FActorPoolClassConfig : public FPoolClassConfigBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool")
	TSoftClassPtr<AActor> ActorClass;
};

USTRUCT(BlueprintType)
struct OBJECTPOOLRUNTIME_API FObjectPoolClassConfig : public FPoolClassConfigBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool", meta = (MetaClass = "/Script/CoreUObject.Object"))
	FSoftClassPath ObjectClass;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool|Network")
	bool bRegisterAsReplicatedSubObject = false;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool|Network", meta = (EditCondition = "bRegisterAsReplicatedSubObject"))
	EObjectPoolRemoteSubObjectReleasePolicy RemoteReleasePolicy = EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica;
};

USTRUCT(BlueprintType)
struct OBJECTPOOLRUNTIME_API FComponentPoolClassConfig : public FPoolClassConfigBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool", meta = (MetaClass = "/Script/Engine.ActorComponent"))
	FSoftClassPath ComponentClass;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool|Network")
	bool bReplicateComponent = false;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool|Network", meta = (EditCondition = "bReplicateComponent"))
	EObjectPoolRemoteSubObjectReleasePolicy RemoteReleasePolicy = EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica;
};

USTRUCT(BlueprintType)
struct OBJECTPOOLRUNTIME_API FPoolModeConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool")
	TArray<FActorPoolClassConfig> ActorPools;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool")
	TArray<FObjectPoolClassConfig> ObjectPools;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Object Pool")
	TArray<FComponentPoolClassConfig> ComponentPools;
};

USTRUCT(BlueprintType)
struct OBJECTPOOLRUNTIME_API FActorPoolAcquireContext
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Object Pool")
	FTransform Transform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Object Pool")
	TObjectPtr<AActor> Owner = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Object Pool")
	TObjectPtr<APawn> Instigator = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Object Pool")
	FGameplayTag ModeTag;
};
