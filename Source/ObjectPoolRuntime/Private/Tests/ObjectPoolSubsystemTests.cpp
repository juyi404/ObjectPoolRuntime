#if WITH_DEV_AUTOMATION_TESTS

#include "ObjectPoolSettings.h"
#include "ObjectPoolSubsystem.h"
#include "ObjectPoolTestObject.h"
#include "ObjectPoolTestComponent.h"
#include "PooledActorNetworkStateComponent.h"
#include "ObjectPoolTestNetworkActor.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "LatentActions.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "TimerManager.h"
#include "NativeGameplayTags.h"
#include "UObject/UObjectGlobals.h"

namespace ObjectPoolSubsystemTests
{
	class FPendingPoolTestAction final : public FPendingLatentAction
	{
	public:
		virtual void UpdateOperation(FLatentResponse& Response) override {}
	};

	UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_ObjectPoolTestModeA, "ObjectPool.Test.ModeA");
	UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_ObjectPoolTestModeB, "ObjectPool.Test.ModeB");

	UWorld* CreateTestWorld(const FName Name)
	{
		const UWorld::InitializationValues Values = UWorld::InitializationValues()
			.AllowAudioPlayback(false)
			.CreateAISystem(false)
			.CreateNavigation(false)
			.CreatePhysicsScene(false)
			.RequiresHitProxies(false)
			.ShouldSimulatePhysics(false)
			.SetTransactional(false);
		UWorld* World = UWorld::CreateWorld(
			EWorldType::Game,
			false,
			Name,
			nullptr,
			true,
			ERHIFeatureLevel::Num,
			&Values);
		if (World != nullptr && GEngine != nullptr)
		{
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
		}
		return World;
	}

	void DestroyTestWorld(UWorld* World)
	{
		if (World == nullptr)
		{
			return;
		}
		if (GEngine != nullptr)
		{
			GEngine->ShutdownWorldNetDriver(World);
		}
		World->DestroyWorld(true);
		if (GEngine != nullptr)
		{
			GEngine->DestroyWorldContext(World);
		}
		if (World->IsRooted())
		{
			World->RemoveFromRoot();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolConfigurationValidationTest,
	"Pool.ObjectPool.Settings.ConfigurationValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolConfigurationValidationTest::RunTest(const FString& Parameters)
{
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const FPoolModeConfig PreviousDefaultModeConfig = Settings->DefaultModeConfig;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->DefaultModeConfig = FPoolModeConfig();
	Settings->ModeConfigs.Reset();
	FString Error;
	TestTrue(TEXT("Empty generic configuration is valid"), Settings->ValidateSettings(&Error));

	FActorPoolClassConfig InvalidPrewarm;
	InvalidPrewarm.ActorClass = AActor::StaticClass();
	InvalidPrewarm.MaxPoolSize = 1;
	InvalidPrewarm.EditorPreallocateCount = 2;
	Settings->DefaultModeConfig.ActorPools.Add(InvalidPrewarm);
	TestFalse(TEXT("Prewarm count above MaxPoolSize is rejected"), Settings->ValidateSettings(&Error));
	Settings->DefaultModeConfig = FPoolModeConfig();

	FActorPoolClassConfig FirstActor;
	FirstActor.ActorClass = AActor::StaticClass();
	FActorPoolClassConfig DuplicateActor = FirstActor;
	Settings->DefaultModeConfig.ActorPools = { FirstActor, DuplicateActor };
	TestFalse(TEXT("Duplicate Actor classes are rejected"), Settings->ValidateSettings(&Error));
	Settings->DefaultModeConfig = FPoolModeConfig();

	FObjectPoolClassConfig InvalidObject;
	InvalidObject.ObjectClass = FSoftClassPath(AActor::StaticClass());
	Settings->DefaultModeConfig.ObjectPools.Add(InvalidObject);
	TestFalse(TEXT("Actor classes in the UObject pool are rejected"), Settings->ValidateSettings(&Error));

	Settings->DefaultModeConfig = PreviousDefaultModeConfig;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolSubsystemActorLifecycleTest,
	"Pool.ObjectPool.Subsystem.ActorLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolSubsystemActorLifecycleTest::RunTest(const FString& Parameters)
{
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	FActorPoolClassConfig ActorConfig;
	ActorConfig.ActorClass = AActor::StaticClass();
	ActorConfig.MaxPoolSize = 2;
	ActorConfig.bAllowRuntimeGrowth = true;
	FPoolModeConfig ModeConfig;
	ModeConfig.ActorPools.Add(ActorConfig);
	Settings->ModeConfigs.Add(FGameplayTag(), ModeConfig);

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolActorLifecycleWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	TestNotNull(TEXT("World creates the object pool subsystem"), Subsystem);

	AActor* First = Subsystem != nullptr
		? Subsystem->SpawnActorFromPool(AActor::StaticClass(), FTransform(FVector(100.0, 0.0, 0.0)))
		: nullptr;
	TestNotNull(TEXT("Configured actor can be created"), First);
	if (Subsystem != nullptr && First != nullptr)
	{
		TestEqual(TEXT("New actor is active"), Subsystem->GetActorPoolState(First), EObjectPoolEntryState::Active);
		TestTrue(TEXT("Active actor can be released"), Subsystem->ReleaseActorToPool(First));
		TestEqual(TEXT("Released actor is inactive"), Subsystem->GetActorPoolState(First), EObjectPoolEntryState::Inactive);
		TestTrue(TEXT("Released actor is hidden"), First->IsHidden());
		TestFalse(TEXT("Released actor collision is disabled"), First->GetActorEnableCollision());

		AActor* Reused = Subsystem->SpawnActorFromPool(AActor::StaticClass(), FTransform(FVector(200.0, 0.0, 0.0)));
		TestEqual(TEXT("Acquire reuses the same actor"), Reused, First);
		TestEqual(TEXT("Reused actor is active"), Subsystem->GetActorPoolState(Reused), EObjectPoolEntryState::Active);

		const FObjectPoolClassStats Stats = Subsystem->GetActorPoolStats(AActor::StaticClass());
		TestEqual(TEXT("Only one actor was created"), Stats.Created, 1);
		TestEqual(TEXT("Second acquire was a pool hit"), Stats.PoolHits, 1);
		TestTrue(TEXT("Subsystem invariants hold"), Subsystem->Validate());
		const FString DebugReport = Subsystem->BuildDebugReport();
		TestTrue(TEXT("Debug report names the test world"), DebugReport.Contains(TEXT("ObjectPoolActorLifecycleWorld")));
		TestTrue(TEXT("Debug report includes hit statistics"), DebugReport.Contains(TEXT("Hits=1")));
	}

	if (World != nullptr)
	{
		ObjectPoolSubsystemTests::DestroyTestWorld(World);
	}

	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolWorldIsolationAndTeardownTest,
	"Pool.ObjectPool.Subsystem.WorldIsolationAndTeardown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolWorldIsolationAndTeardownTest::RunTest(const FString& Parameters)
{
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	FActorPoolClassConfig ActorConfig;
	ActorConfig.ActorClass = AActor::StaticClass();
	ActorConfig.MaxPoolSize = 1;
	ActorConfig.bAllowRuntimeGrowth = true;
	FPoolModeConfig ModeConfig;
	ModeConfig.ActorPools.Add(ActorConfig);
	Settings->ModeConfigs.Add(FGameplayTag(), ModeConfig);

	UWorld* WorldA = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolTeardownWorldA"));
	UWorld* WorldB = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolTeardownWorldB"));
	UObjectPoolSubsystem* PoolA = WorldA != nullptr ? WorldA->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	UObjectPoolSubsystem* PoolB = WorldB != nullptr ? WorldB->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	TestNotNull(TEXT("First world owns an object pool subsystem"), PoolA);
	TestNotNull(TEXT("Second world owns an object pool subsystem"), PoolB);
	TestNotEqual(TEXT("Worlds do not share an object pool subsystem"), PoolA, PoolB);

	AActor* ActorA = PoolA != nullptr ? PoolA->SpawnActorFromPool(AActor::StaticClass(), FTransform::Identity) : nullptr;
	AActor* ActorB = PoolB != nullptr ? PoolB->SpawnActorFromPool(AActor::StaticClass(), FTransform(FVector(100.0, 0.0, 0.0))) : nullptr;
	TestNotNull(TEXT("First world creates its own pooled actor"), ActorA);
	TestNotNull(TEXT("Second world creates its own pooled actor"), ActorB);
	TestNotEqual(TEXT("World pools never share actor instances"), ActorA, ActorB);

	TWeakObjectPtr<UWorld> WeakWorldA = WorldA;
	TWeakObjectPtr<UObjectPoolSubsystem> WeakPoolA = PoolA;
	TWeakObjectPtr<AActor> WeakActorA = ActorA;
	if (PoolA != nullptr && ActorA != nullptr)
	{
		TestTrue(TEXT("First world can release before map unload"), PoolA->ReleaseActorToPool(ActorA));
	}

	ObjectPoolSubsystemTests::DestroyTestWorld(WorldA);
	WorldA = nullptr;
	PoolA = nullptr;
	ActorA = nullptr;
	CollectGarbage(RF_NoFlags);

	TestFalse(TEXT("Unloaded world is collectable"), WeakWorldA.IsValid());
	TestFalse(TEXT("Unloaded world's subsystem is collectable"), WeakPoolA.IsValid());
	TestFalse(TEXT("Unloaded world's pooled actor is destroyed"), WeakActorA.IsValid());

	if (PoolB != nullptr && ActorB != nullptr)
	{
		TestEqual(TEXT("Other world's actor remains active"), PoolB->GetActorPoolState(ActorB), EObjectPoolEntryState::Active);
		TestTrue(TEXT("Other world's pool remains valid after peer unload"), PoolB->Validate());
		TestTrue(TEXT("Other world can still release its actor"), PoolB->ReleaseActorToPool(ActorB));
		AActor* ReusedB = PoolB->SpawnActorFromPool(AActor::StaticClass(), FTransform(FVector(200.0, 0.0, 0.0)));
		TestEqual(TEXT("Other world still reuses its own actor"), ReusedB, ActorB);
	}

	ObjectPoolSubsystemTests::DestroyTestWorld(WorldB);
	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolSubsystemUObjectLifecycleTest,
	"Pool.ObjectPool.Subsystem.UObjectLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolSubsystemUObjectLifecycleTest::RunTest(const FString& Parameters)
{
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	FObjectPoolClassConfig ObjectConfig;
	ObjectConfig.ObjectClass = FSoftClassPath(UObjectPoolTestObject::StaticClass());
	ObjectConfig.MaxPoolSize = 2;
	ObjectConfig.bAllowRuntimeGrowth = true;
	FPoolModeConfig ModeConfig;
	ModeConfig.ObjectPools.Add(ObjectConfig);
	Settings->ModeConfigs.Add(FGameplayTag(), ModeConfig);

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolUObjectLifecycleWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	TestNotNull(TEXT("World creates the object pool subsystem"), Subsystem);

	UObject* FirstOuter = World != nullptr ? NewObject<UObjectPoolTestObject>(World) : nullptr;
	UObjectPoolTestObject* First = Subsystem != nullptr
		? Cast<UObjectPoolTestObject>(Subsystem->GetObjectFromPool(FirstOuter, UObjectPoolTestObject::StaticClass()))
		: nullptr;
	TestNotNull(TEXT("Configured UObject can be created"), First);
	if (Subsystem != nullptr && First != nullptr)
	{
		TestEqual(TEXT("Object is assigned to the requested Outer"), First->GetOuter(), FirstOuter);
		TestEqual(TEXT("Created callback ran once"), First->CreatedCount, 1);
		TestEqual(TEXT("Acquire callback ran once"), First->AcquiredCount, 1);
		FTimerHandle ObjectTimer;
		World->GetTimerManager().SetTimer(
			ObjectTimer, First, &UObjectPoolTestObject::TestTimerCallback, 60.0f, false);
		First->PoolForReentrantRelease = Subsystem;
		First->bAttemptReentrantRelease = true;
		TestTrue(TEXT("Object can be released"), Subsystem->ReleaseObjectToPool(First));
		TestEqual(TEXT("Release callback observes the releasing transition"),
			First->StateObservedDuringRelease, EObjectPoolEntryState::Releasing);
		TestFalse(TEXT("Recursive object release is rejected"), First->bReentrantReleaseResult);
		TestNull(TEXT("Object acquire during release is rejected"), First->ReentrantAcquireResult.Get());
		TestFalse(TEXT("Framework recovery clears object timers"), World->GetTimerManager().IsTimerActive(ObjectTimer));
		TestEqual(TEXT("Released object is inactive"), Subsystem->GetObjectPoolState(First), EObjectPoolEntryState::Inactive);
		TestNotEqual(TEXT("Released object no longer uses the business Outer"), First->GetOuter(), FirstOuter);
		TestEqual(TEXT("Release callback ran once"), First->ReleasedCount, 1);

		CollectGarbage(RF_NoFlags);
		TestTrue(TEXT("Pool strong references survive forced GC"), IsValid(First));

		UObject* SecondOuter = NewObject<UObjectPoolTestObject>(World);
		UObjectPoolTestObject* Reused = Cast<UObjectPoolTestObject>(
			Subsystem->GetObjectFromPool(SecondOuter, UObjectPoolTestObject::StaticClass()));
		TestEqual(TEXT("Acquire reuses the same UObject"), Reused, First);
		TestEqual(TEXT("Reused object receives the new Outer"), Reused->GetOuter(), SecondOuter);
		TestEqual(TEXT("Created callback is not repeated"), Reused->CreatedCount, 1);
		TestEqual(TEXT("Acquire callback runs for every reuse"), Reused->AcquiredCount, 2);
		TestTrue(TEXT("Subsystem invariants hold"), Subsystem->Validate());
	}

	if (World != nullptr)
	{
		ObjectPoolSubsystemTests::DestroyTestWorld(World);
	}

	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolSubsystemComponentLifecycleTest,
	"Pool.ObjectPool.Subsystem.ComponentLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolSubsystemComponentLifecycleTest::RunTest(const FString& Parameters)
{
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	FComponentPoolClassConfig ComponentConfig;
	ComponentConfig.ComponentClass = FSoftClassPath(UObjectPoolTestComponent::StaticClass());
	ComponentConfig.MaxPoolSize = 2;
	ComponentConfig.bAllowRuntimeGrowth = true;
	FPoolModeConfig ModeConfig;
	ModeConfig.ComponentPools.Add(ComponentConfig);
	Settings->ModeConfigs.Add(FGameplayTag(), ModeConfig);

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolComponentLifecycleWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	AActor* FirstOwner = World != nullptr ? World->SpawnActor<AActor>() : nullptr;
	UObjectPoolTestComponent* First = Subsystem != nullptr
		? Cast<UObjectPoolTestComponent>(Subsystem->GetComponentFromPool(FirstOwner, UObjectPoolTestComponent::StaticClass()))
		: nullptr;

	TestNotNull(TEXT("Configured component can be created"), First);
	if (Subsystem != nullptr && First != nullptr)
	{
		TestEqual(TEXT("Component receives requested owner"), First->GetOwner(), FirstOwner);
		TestTrue(TEXT("Acquired component is registered"), First->IsRegistered());
		TestEqual(TEXT("Created callback ran once"), First->CreatedCount, 1);
		TestEqual(TEXT("Acquire callback ran once"), First->AcquiredCount, 1);
		FTimerHandle ComponentTimer;
		World->GetTimerManager().SetTimer(
			ComponentTimer, First, &UObjectPoolTestComponent::TestTimerCallback, 60.0f, false);
		First->PoolForReentrantRelease = Subsystem;
		First->bAttemptReentrantRelease = true;
		TestTrue(TEXT("Component can be released"), Subsystem->ReleaseComponentToPool(First));
		TestEqual(TEXT("Component release callback observes the releasing transition"),
			First->StateObservedDuringRelease, EObjectPoolEntryState::Releasing);
		TestFalse(TEXT("Recursive component release is rejected"), First->bReentrantReleaseResult);
		TestNull(TEXT("Component acquire during release is rejected"), First->ReentrantAcquireResult.Get());
		TestFalse(TEXT("Framework recovery clears component timers"), World->GetTimerManager().IsTimerActive(ComponentTimer));
		TestEqual(TEXT("Released component is inactive"), Subsystem->GetComponentPoolState(First), EObjectPoolEntryState::Inactive);
		TestFalse(TEXT("Released component is unregistered"), First->IsRegistered());
		TestNotEqual(TEXT("Released component leaves business owner"), First->GetOwner(), FirstOwner);
		TestEqual(TEXT("Release callback ran once"), First->ReleasedCount, 1);

		CollectGarbage(RF_NoFlags);
		TestTrue(TEXT("Pooled component survives forced GC"), IsValid(First));

		AActor* SecondOwner = World->SpawnActor<AActor>();
		UObjectPoolTestComponent* Reused = Cast<UObjectPoolTestComponent>(
			Subsystem->GetComponentFromPool(SecondOwner, UObjectPoolTestComponent::StaticClass()));
		TestEqual(TEXT("Acquire reuses the same component"), Reused, First);
		TestEqual(TEXT("Reused component receives new owner"), Reused->GetOwner(), SecondOwner);
		TestTrue(TEXT("Reused component is registered"), Reused->IsRegistered());
		TestEqual(TEXT("Created callback is not repeated"), Reused->CreatedCount, 1);
		TestEqual(TEXT("Acquire callback runs for every reuse"), Reused->AcquiredCount, 2);
		TestTrue(TEXT("Subsystem invariants hold"), Subsystem->Validate());
	}

	if (World != nullptr)
	{
		ObjectPoolSubsystemTests::DestroyTestWorld(World);
	}

	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolRecoveryPolicyMatrixTest,
	"Pool.ObjectPool.Subsystem.RecoveryPolicyMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolRecoveryPolicyMatrixTest::RunTest(const FString& Parameters)
{
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const FPoolModeConfig PreviousDefaultModeConfig = Settings->DefaultModeConfig;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	const EObjectPoolRecoveryPolicy Policies[] = {
		EObjectPoolRecoveryPolicy::BusinessCallbacksOnly,
		EObjectPoolRecoveryPolicy::ResetFrameworkState,
		EObjectPoolRecoveryPolicy::Full
	};
	for (int32 PolicyIndex = 0; PolicyIndex < UE_ARRAY_COUNT(Policies); ++PolicyIndex)
	{
		const EObjectPoolRecoveryPolicy Policy = Policies[PolicyIndex];
		FPoolModeConfig ModeConfig;
		FActorPoolClassConfig ActorConfig;
		ActorConfig.ActorClass = AObjectPoolTestNetworkActor::StaticClass();
		ActorConfig.MaxPoolSize = 1;
		ActorConfig.RecoveryPolicy = Policy;
		ModeConfig.ActorPools.Add(ActorConfig);
		FObjectPoolClassConfig ObjectConfig;
		ObjectConfig.ObjectClass = FSoftClassPath(UObjectPoolTestObject::StaticClass());
		ObjectConfig.MaxPoolSize = 1;
		ObjectConfig.RecoveryPolicy = Policy;
		ModeConfig.ObjectPools.Add(ObjectConfig);
		FComponentPoolClassConfig ComponentConfig;
		ComponentConfig.ComponentClass = FSoftClassPath(UObjectPoolTestComponent::StaticClass());
		ComponentConfig.MaxPoolSize = 1;
		ComponentConfig.RecoveryPolicy = Policy;
		ModeConfig.ComponentPools.Add(ComponentConfig);
		Settings->DefaultModeConfig = ModeConfig;

		const FString WorldName = FString::Printf(TEXT("ObjectPoolRecoveryPolicyWorld_%d"), PolicyIndex);
		UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(*WorldName);
		UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
		AObjectPoolTestNetworkActor* Actor = Subsystem != nullptr
			? Cast<AObjectPoolTestNetworkActor>(Subsystem->SpawnActorFromPool(AObjectPoolTestNetworkActor::StaticClass(), FTransform::Identity))
			: nullptr;
		UObjectPoolTestObject* Outer = World != nullptr ? NewObject<UObjectPoolTestObject>(World) : nullptr;
		UObjectPoolTestObject* Object = Subsystem != nullptr
			? Cast<UObjectPoolTestObject>(Subsystem->GetObjectFromPool(Outer, UObjectPoolTestObject::StaticClass()))
			: nullptr;
		AActor* ComponentOwner = World != nullptr ? World->SpawnActor<AActor>() : nullptr;
		UObjectPoolTestComponent* Component = Subsystem != nullptr
			? Cast<UObjectPoolTestComponent>(Subsystem->GetComponentFromPool(ComponentOwner, UObjectPoolTestComponent::StaticClass()))
			: nullptr;
		TestNotNull(FString::Printf(TEXT("Policy %d creates Actor"), PolicyIndex), Actor);
		TestNotNull(FString::Printf(TEXT("Policy %d creates UObject"), PolicyIndex), Object);
		TestNotNull(FString::Printf(TEXT("Policy %d creates Component"), PolicyIndex), Component);

		if (World != nullptr && Subsystem != nullptr && Actor != nullptr && Object != nullptr && Component != nullptr)
		{
			FTimerHandle ActorTimer;
			FTimerHandle ObjectTimer;
			FTimerHandle ComponentTimer;
			World->GetTimerManager().SetTimer(ActorTimer, Actor, &AObjectPoolTestNetworkActor::TestTimerCallback, 60.0f, false);
			World->GetTimerManager().SetTimer(ObjectTimer, Object, &UObjectPoolTestObject::TestTimerCallback, 60.0f, false);
			World->GetTimerManager().SetTimer(ComponentTimer, Component, &UObjectPoolTestComponent::TestTimerCallback, 60.0f, false);

			constexpr int32 ActorLatentId = 1001;
			constexpr int32 ObjectLatentId = 1002;
			constexpr int32 ComponentLatentId = 1003;
			FLatentActionManager& LatentManager = World->GetLatentActionManager();
			LatentManager.AddNewAction(Actor, ActorLatentId, new ObjectPoolSubsystemTests::FPendingPoolTestAction());
			LatentManager.AddNewAction(Object, ObjectLatentId, new ObjectPoolSubsystemTests::FPendingPoolTestAction());
			LatentManager.AddNewAction(Component, ComponentLatentId, new ObjectPoolSubsystemTests::FPendingPoolTestAction());

			TestTrue(TEXT("Actor release succeeds for recovery policy"), Subsystem->ReleaseActorToPool(Actor));
			TestTrue(TEXT("UObject release succeeds for recovery policy"), Subsystem->ReleaseObjectToPool(Object));
			TestTrue(TEXT("Component release succeeds for recovery policy"), Subsystem->ReleaseComponentToPool(Component));
			// RemoveActionsForObject is applied by the latent manager at its next processing boundary.
			LatentManager.ProcessLatentActions(nullptr, 0.0f);

			const bool bExpectTimers = Policy == EObjectPoolRecoveryPolicy::BusinessCallbacksOnly;
			TestEqual(TEXT("Actor timer follows recovery policy"), World->GetTimerManager().IsTimerActive(ActorTimer), bExpectTimers);
			TestEqual(TEXT("UObject timer follows recovery policy"), World->GetTimerManager().IsTimerActive(ObjectTimer), bExpectTimers);
			TestEqual(TEXT("Component timer follows recovery policy"), World->GetTimerManager().IsTimerActive(ComponentTimer), bExpectTimers);

			const bool bExpectLatentActions = Policy != EObjectPoolRecoveryPolicy::Full;
			TestEqual(TEXT("Actor latent action follows recovery policy"),
				LatentManager.FindExistingAction<ObjectPoolSubsystemTests::FPendingPoolTestAction>(Actor, ActorLatentId) != nullptr,
				bExpectLatentActions);
			TestEqual(TEXT("UObject latent action follows recovery policy"),
				LatentManager.FindExistingAction<ObjectPoolSubsystemTests::FPendingPoolTestAction>(Object, ObjectLatentId) != nullptr,
				bExpectLatentActions);
			TestEqual(TEXT("Component latent action follows recovery policy"),
				LatentManager.FindExistingAction<ObjectPoolSubsystemTests::FPendingPoolTestAction>(Component, ComponentLatentId) != nullptr,
				bExpectLatentActions);
			TestTrue(TEXT("Recovery policy preserves pool invariants"), Subsystem->Validate());
		}

		ObjectPoolSubsystemTests::DestroyTestWorld(World);
	}

	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->DefaultModeConfig = PreviousDefaultModeConfig;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolCallbackInvalidationTest,
	"Pool.ObjectPool.Subsystem.CallbackInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolCallbackInvalidationTest::RunTest(const FString& Parameters)
{
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const FPoolModeConfig PreviousDefaultModeConfig = Settings->DefaultModeConfig;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();
	FPoolModeConfig ModeConfig;
	FActorPoolClassConfig ActorConfig;
	ActorConfig.ActorClass = AObjectPoolTestNetworkActor::StaticClass();
	ActorConfig.MaxPoolSize = 1;
	ModeConfig.ActorPools.Add(ActorConfig);
	FObjectPoolClassConfig ObjectConfig;
	ObjectConfig.ObjectClass = FSoftClassPath(UObjectPoolTestObject::StaticClass());
	ObjectConfig.MaxPoolSize = 1;
	ModeConfig.ObjectPools.Add(ObjectConfig);
	FComponentPoolClassConfig ComponentConfig;
	ComponentConfig.ComponentClass = FSoftClassPath(UObjectPoolTestComponent::StaticClass());
	ComponentConfig.MaxPoolSize = 1;
	ModeConfig.ComponentPools.Add(ComponentConfig);
	Settings->DefaultModeConfig = ModeConfig;

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolCallbackInvalidationWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	AObjectPoolTestNetworkActor* Actor = Subsystem != nullptr
		? Cast<AObjectPoolTestNetworkActor>(Subsystem->SpawnActorFromPool(AObjectPoolTestNetworkActor::StaticClass(), FTransform::Identity))
		: nullptr;
	TestNotNull(TEXT("Callback invalidation test creates actor"), Actor);
	if (Subsystem != nullptr && Actor != nullptr)
	{
		Actor->PoolForReentrantRelease = Subsystem;
		Actor->bAttemptReentrantRelease = true;
		TestTrue(TEXT("Actor release succeeds while rejecting reentrant mutations"), Subsystem->ReleaseActorToPool(Actor));
		TestEqual(TEXT("Actor callback observes Releasing"), Actor->StateObservedDuringRelease, EObjectPoolEntryState::Releasing);
		TestFalse(TEXT("Recursive actor release is rejected"), Actor->bReentrantReleaseResult);
		TestNull(TEXT("Actor acquire during release is rejected"), Actor->ReentrantAcquireResult.Get());

		Actor = Cast<AObjectPoolTestNetworkActor>(
			Subsystem->SpawnActorFromPool(AObjectPoolTestNetworkActor::StaticClass(), FTransform::Identity));
		Actor->bAttemptReentrantRelease = false;
		Actor->bDestroyOnRelease = true;
		TestFalse(TEXT("Actor destroyed by its callback is removed instead of returned inactive"),
			Subsystem->ReleaseActorToPool(Actor));
	}

	UObjectPoolTestObject* Outer = World != nullptr ? NewObject<UObjectPoolTestObject>(World) : nullptr;
	UObjectPoolTestObject* Object = Subsystem != nullptr
		? Cast<UObjectPoolTestObject>(Subsystem->GetObjectFromPool(Outer, UObjectPoolTestObject::StaticClass()))
		: nullptr;
	TestNotNull(TEXT("Callback invalidation test creates UObject"), Object);
	if (Subsystem != nullptr && Object != nullptr)
	{
		Object->bMarkGarbageOnRelease = true;
		TestFalse(TEXT("Garbage-marked UObject is removed instead of returned inactive"),
			Subsystem->ReleaseObjectToPool(Object));
	}

	AActor* ComponentOwner = World != nullptr ? World->SpawnActor<AActor>() : nullptr;
	UObjectPoolTestComponent* Component = Subsystem != nullptr
		? Cast<UObjectPoolTestComponent>(Subsystem->GetComponentFromPool(ComponentOwner, UObjectPoolTestComponent::StaticClass()))
		: nullptr;
	TestNotNull(TEXT("Callback invalidation test creates component"), Component);
	if (Subsystem != nullptr && Component != nullptr)
	{
		Component->bMarkGarbageOnRelease = true;
		TestFalse(TEXT("Garbage-marked component is removed instead of returned inactive"),
			Subsystem->ReleaseComponentToPool(Component));
	}
	if (Subsystem != nullptr)
	{
		TestTrue(TEXT("Callback invalidation leaves all pool invariants valid"), Subsystem->Validate());
	}

	ObjectPoolSubsystemTests::DestroyTestWorld(World);
	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->DefaultModeConfig = PreviousDefaultModeConfig;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPooledActorNetworkStateTest,
	"Pool.ObjectPool.Network.StateGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPooledActorNetworkStateTest::RunTest(const FString& Parameters)
{
	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolNetworkStateWorld"));
	AObjectPoolTestNetworkActor* Actor = World != nullptr ? World->SpawnActor<AObjectPoolTestNetworkActor>() : nullptr;
	TestNotNull(TEXT("Network test actor was created"), Actor);

	if (Actor != nullptr)
	{
		UPooledActorNetworkStateComponent* State = Actor->GetNetworkState();
		TestNotNull(TEXT("Network state is a replicated default subobject"), State);
		IPoolableActorInterface* NativePoolInterface = Cast<IPoolableActorInterface>(Actor);
		TestNotNull(TEXT("Network test actor exposes its native pool interface"), NativePoolInterface);
		if (NativePoolInterface != nullptr)
		{
			NativePoolInterface->OnPoolReleaseClient_Implementation();
			TestEqual(TEXT("Native client callback implementation is callable"), Actor->ClientReleaseCount, 1);
			Actor->ClientReleaseCount = 0;
		}
		if (State == nullptr)
		{
			ObjectPoolSubsystemTests::DestroyTestWorld(World);
			return false;
		}

		TestTrue(TEXT("Network state starts active"), State->IsPoolActive());
		TestEqual(TEXT("Initial generation is zero"), State->GetPoolGeneration(), 0);
		State->SetPoolActiveFromServer(false);
		TestFalse(TEXT("Server can mark state inactive"), State->IsPoolActive());
		State->ProcessEvent(State->FindFunctionChecked(TEXT("OnRep_PoolState")), nullptr);
		TestEqual(TEXT("Replicated release invokes the client callback"), Actor->ClientReleaseCount, 1);
		TestTrue(TEXT("Replicated release hides the client actor"), Actor->IsHidden());
		TestFalse(TEXT("Replicated release disables client collision"), Actor->GetActorEnableCollision());
		TestFalse(TEXT("Replicated release disables client tick"), Actor->IsActorTickEnabled());
		FActorPoolAcquireContext AcquireContext;
		AcquireContext.Transform = FTransform(FVector(50.0, 0.0, 0.0));
		State->SetPoolAcquiredFromServer(AcquireContext);
		TestTrue(TEXT("Server can reactivate state"), State->IsPoolActive());
		State->ProcessEvent(State->FindFunctionChecked(TEXT("OnRep_PoolState")), nullptr);
		TestEqual(TEXT("Replicated acquire invokes the client callback"), Actor->ClientAcquireCount, 1);
		TestEqual(TEXT("Replicated acquire delivers the atomic context transform"),
			Actor->LastClientAcquireContext.Transform.GetLocation(), FVector(50.0, 0.0, 0.0));
		TestFalse(TEXT("Replicated acquire shows the client actor"), Actor->IsHidden());
		TestTrue(TEXT("Replicated acquire enables client collision"), Actor->GetActorEnableCollision());
		TestTrue(TEXT("Replicated acquire enables client tick"), Actor->IsActorTickEnabled());
		TestEqual(TEXT("Activation increments generation"), State->GetPoolGeneration(), 1);
		State->SetPoolActiveFromServer(true);
		TestEqual(TEXT("Duplicate activation does not increment generation"), State->GetPoolGeneration(), 1);

		// Simulate Release and Acquire being collapsed before the client receives either transition.
		State->SetPoolActiveFromServer(false);
		FActorPoolAcquireContext CoalescedContext;
		CoalescedContext.Transform = FTransform(FVector(125.0, 0.0, 0.0));
		State->SetPoolAcquiredFromServer(CoalescedContext);
		State->ProcessEvent(State->FindFunctionChecked(TEXT("OnRep_PoolState")), nullptr);
		TestEqual(TEXT("Coalesced reuse advances the generation"), State->GetPoolGeneration(), 2);
		TestEqual(TEXT("Coalesced reuse reconstructs the missing client release"), Actor->ClientReleaseCount, 2);
		TestEqual(TEXT("Coalesced reuse still invokes the next client acquire"), Actor->ClientAcquireCount, 2);
		TestEqual(TEXT("Coalesced reuse delivers the newest atomic context"),
			Actor->LastClientAcquireContext.Transform.GetLocation(), FVector(125.0, 0.0, 0.0));
	}

	if (World != nullptr)
	{
		ObjectPoolSubsystemTests::DestroyTestWorld(World);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolReplicatedSubObjectRegistrationTest,
	"Pool.ObjectPool.Network.ReplicatedSubObjectRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolReplicatedSubObjectRegistrationTest::RunTest(const FString& Parameters)
{
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	FObjectPoolClassConfig ObjectConfig;
	ObjectConfig.ObjectClass = FSoftClassPath(UObjectPoolTestObject::StaticClass());
	ObjectConfig.MaxPoolSize = 2;
	ObjectConfig.bAllowRuntimeGrowth = true;
	ObjectConfig.bRegisterAsReplicatedSubObject = true;
	ObjectConfig.RemoteReleasePolicy = EObjectPoolRemoteSubObjectReleasePolicy::DestroyRemoteReplica;
	FPoolModeConfig ModeConfig;
	ModeConfig.ObjectPools.Add(ObjectConfig);
	Settings->ModeConfigs.Add(FGameplayTag(), ModeConfig);

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolReplicatedSubObjectWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	AObjectPoolTestNetworkActor* Owner = World != nullptr ? World->SpawnActor<AObjectPoolTestNetworkActor>() : nullptr;
	AObjectPoolTestNetworkActor* SecondOwner = World != nullptr ? World->SpawnActor<AObjectPoolTestNetworkActor>() : nullptr;
	UObjectPoolTestObject* Object = Subsystem != nullptr
		? Cast<UObjectPoolTestObject>(Subsystem->GetObjectFromPool(Owner, UObjectPoolTestObject::StaticClass()))
		: nullptr;

	TestNotNull(TEXT("Replicated pooled UObject was created"), Object);
	if (Owner != nullptr && Object != nullptr && Subsystem != nullptr)
	{
		TestTrue(TEXT("UObject is registered with its replicated owner"), Owner->IsReplicatedSubObjectRegistered(Object));
		TestTrue(TEXT("Replicated UObject can be released"), Subsystem->ReleaseObjectToPool(Object));
		TestFalse(TEXT("Released UObject is removed from replicated subobject list"), Owner->IsReplicatedSubObjectRegistered(Object));
		UObjectPoolTestObject* Reused = Cast<UObjectPoolTestObject>(
			Subsystem->GetObjectFromPool(SecondOwner, UObjectPoolTestObject::StaticClass()));
		TestEqual(TEXT("Replicated UObject is reused across owners"), Reused, Object);
		TestFalse(TEXT("Previous owner does not retain the reused UObject"), Owner->IsReplicatedSubObjectRegistered(Reused));
		TestTrue(TEXT("New owner registers the reused UObject"), SecondOwner->IsReplicatedSubObjectRegistered(Reused));
		TestTrue(TEXT("Cross-owner UObject reuse preserves pool invariants"), Subsystem->Validate());
	}

	if (World != nullptr)
	{
		ObjectPoolSubsystemTests::DestroyTestWorld(World);
	}
	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolReplicatedComponentRegistrationTest,
	"Pool.ObjectPool.Network.ReplicatedComponentRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolReplicatedComponentRegistrationTest::RunTest(const FString& Parameters)
{
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	FComponentPoolClassConfig ComponentConfig;
	ComponentConfig.ComponentClass = FSoftClassPath(UObjectPoolTestComponent::StaticClass());
	ComponentConfig.MaxPoolSize = 2;
	ComponentConfig.bAllowRuntimeGrowth = true;
	ComponentConfig.bReplicateComponent = true;
	FPoolModeConfig ModeConfig;
	ModeConfig.ComponentPools.Add(ComponentConfig);
	Settings->ModeConfigs.Add(FGameplayTag(), ModeConfig);

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolReplicatedComponentWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	AObjectPoolTestNetworkActor* Owner = World != nullptr ? World->SpawnActor<AObjectPoolTestNetworkActor>() : nullptr;
	AObjectPoolTestNetworkActor* SecondOwner = World != nullptr ? World->SpawnActor<AObjectPoolTestNetworkActor>() : nullptr;
	UObjectPoolTestComponent* Component = Subsystem != nullptr
		? Cast<UObjectPoolTestComponent>(Subsystem->GetComponentFromPool(Owner, UObjectPoolTestComponent::StaticClass()))
		: nullptr;

	TestNotNull(TEXT("Replicated pooled component was created"), Component);
	if (Owner != nullptr && Component != nullptr && Subsystem != nullptr)
	{
		TestTrue(TEXT("Component replication is enabled on acquire"), Component->GetIsReplicated());
		TestTrue(TEXT("Owner registers the replicated component"), Owner->IsReplicatedActorComponentRegistered(Component));
		TestTrue(TEXT("Replicated component can be released"), Subsystem->ReleaseComponentToPool(Component));
		TestFalse(TEXT("Component replication is disabled on release"), Component->GetIsReplicated());
		TestFalse(TEXT("Owner removes released replicated component"), Owner->IsReplicatedActorComponentRegistered(Component));
		UObjectPoolTestComponent* Reused = Cast<UObjectPoolTestComponent>(
			Subsystem->GetComponentFromPool(SecondOwner, UObjectPoolTestComponent::StaticClass()));
		TestEqual(TEXT("Replicated component is reused across owners"), Reused, Component);
		TestTrue(TEXT("Reused component is moved to the new owner"), Reused->GetOwner() == SecondOwner);
		TestFalse(TEXT("Previous owner does not retain the reused component"), Owner->IsReplicatedActorComponentRegistered(Reused));
		TestTrue(TEXT("New owner registers the reused component"), SecondOwner->IsReplicatedActorComponentRegistered(Reused));
		TestTrue(TEXT("Cross-owner component reuse preserves pool invariants"), Subsystem->Validate());
	}

	if (World != nullptr)
	{
		ObjectPoolSubsystemTests::DestroyTestWorld(World);
	}
	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolUObjectReuseStressTest,
	"Pool.ObjectPool.Stress.UObjectReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolUObjectReuseStressTest::RunTest(const FString& Parameters)
{
	constexpr int32 Iterations = 10000;
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	FObjectPoolClassConfig ObjectConfig;
	ObjectConfig.ObjectClass = FSoftClassPath(UObjectPoolTestObject::StaticClass());
	ObjectConfig.MaxPoolSize = 1;
	ObjectConfig.bAllowRuntimeGrowth = true;
	FPoolModeConfig ModeConfig;
	ModeConfig.ObjectPools.Add(ObjectConfig);
	Settings->ModeConfigs.Add(FGameplayTag(), ModeConfig);

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolUObjectStressWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	UObjectPoolTestObject* OuterA = World != nullptr ? NewObject<UObjectPoolTestObject>(World) : nullptr;
	UObjectPoolTestObject* OuterB = World != nullptr ? NewObject<UObjectPoolTestObject>(World) : nullptr;
	UObjectPoolTestObject* Object = Subsystem != nullptr
		? Cast<UObjectPoolTestObject>(Subsystem->GetObjectFromPool(OuterA, UObjectPoolTestObject::StaticClass()))
		: nullptr;

	const double StartSeconds = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < Iterations && Object != nullptr; ++Index)
	{
		if (!Subsystem->ReleaseObjectToPool(Object))
		{
			AddError(FString::Printf(TEXT("Stress release failed at iteration %d"), Index));
			break;
		}
		UObject* NextOuter = (Index & 1) == 0 ? static_cast<UObject*>(OuterB) : static_cast<UObject*>(OuterA);
		Object = Cast<UObjectPoolTestObject>(Subsystem->GetObjectFromPool(NextOuter, UObjectPoolTestObject::StaticClass()));
		if (Object == nullptr)
		{
			AddError(FString::Printf(TEXT("Stress acquire failed at iteration %d"), Index));
		}
	}
	const double ElapsedMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	AddInfo(FString::Printf(TEXT("10,000 UObject release/acquire cycles completed in %.3f ms"), ElapsedMilliseconds));

	if (Subsystem != nullptr && Object != nullptr)
	{
		const FObjectPoolClassStats Stats = Subsystem->GetObjectPoolStats(UObjectPoolTestObject::StaticClass());
		TestEqual(TEXT("Stress run creates only one pooled object"), Stats.Created, 1);
		TestEqual(TEXT("Every reacquire is a pool hit"), Stats.PoolHits, Iterations);
		TestEqual(TEXT("Acquire callback count matches reuse count"), Object->AcquiredCount, Iterations + 1);
		TestEqual(TEXT("Release callback count matches iteration count"), Object->ReleasedCount, Iterations);
		TestTrue(TEXT("Subsystem invariants hold after stress run"), Subsystem->Validate());
	}

	ObjectPoolSubsystemTests::DestroyTestWorld(World);
	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolActorReuseStressTest,
	"Pool.ObjectPool.Stress.ActorReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolActorReuseStressTest::RunTest(const FString& Parameters)
{
	constexpr int32 Iterations = 5000;
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	FActorPoolClassConfig ActorConfig;
	ActorConfig.ActorClass = AActor::StaticClass();
	ActorConfig.MaxPoolSize = 1;
	ActorConfig.bAllowRuntimeGrowth = true;
	FPoolModeConfig ModeConfig;
	ModeConfig.ActorPools.Add(ActorConfig);
	Settings->ModeConfigs.Add(FGameplayTag(), ModeConfig);

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolActorStressWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	AActor* FirstActor = Subsystem != nullptr
		? Subsystem->SpawnActorFromPool(AActor::StaticClass(), FTransform::Identity)
		: nullptr;
	AActor* Actor = FirstActor;

	const double StartSeconds = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < Iterations && Actor != nullptr; ++Index)
	{
		if (!Subsystem->ReleaseActorToPool(Actor))
		{
			AddError(FString::Printf(TEXT("Actor stress release failed at iteration %d"), Index));
			break;
		}
		Actor = Subsystem->SpawnActorFromPool(
			AActor::StaticClass(),
			FTransform(FVector(static_cast<double>(Index), 0.0, 0.0)));
		if (Actor != FirstActor)
		{
			AddError(FString::Printf(TEXT("Actor stress did not reuse the original instance at iteration %d"), Index));
			break;
		}
	}
	const double ElapsedMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	AddInfo(FString::Printf(TEXT("5,000 Actor release/acquire cycles completed in %.3f ms"), ElapsedMilliseconds));

	if (Subsystem != nullptr && Actor != nullptr)
	{
		const FObjectPoolClassStats Stats = Subsystem->GetActorPoolStats(AActor::StaticClass());
		TestEqual(TEXT("Actor stress creates only one instance"), Stats.Created, 1);
		TestEqual(TEXT("Every actor reacquire is a pool hit"), Stats.PoolHits, Iterations);
		TestTrue(TEXT("Subsystem invariants hold after actor stress"), Subsystem->Validate());
	}

	ObjectPoolSubsystemTests::DestroyTestWorld(World);
	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolComponentReuseStressTest,
	"Pool.ObjectPool.Stress.ComponentReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolComponentReuseStressTest::RunTest(const FString& Parameters)
{
	constexpr int32 Iterations = 5000;
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	FComponentPoolClassConfig ComponentConfig;
	ComponentConfig.ComponentClass = FSoftClassPath(UObjectPoolTestComponent::StaticClass());
	ComponentConfig.MaxPoolSize = 1;
	ComponentConfig.bAllowRuntimeGrowth = true;
	FPoolModeConfig ModeConfig;
	ModeConfig.ComponentPools.Add(ComponentConfig);
	Settings->ModeConfigs.Add(FGameplayTag(), ModeConfig);

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolComponentStressWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	AActor* OwnerA = World != nullptr ? World->SpawnActor<AActor>() : nullptr;
	AActor* OwnerB = World != nullptr ? World->SpawnActor<AActor>() : nullptr;
	UObjectPoolTestComponent* FirstComponent = Subsystem != nullptr
		? Cast<UObjectPoolTestComponent>(Subsystem->GetComponentFromPool(OwnerA, UObjectPoolTestComponent::StaticClass()))
		: nullptr;
	UObjectPoolTestComponent* Component = FirstComponent;

	const double StartSeconds = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < Iterations && Component != nullptr; ++Index)
	{
		if (!Subsystem->ReleaseComponentToPool(Component))
		{
			AddError(FString::Printf(TEXT("Component stress release failed at iteration %d"), Index));
			break;
		}
		AActor* NextOwner = (Index & 1) == 0 ? OwnerB : OwnerA;
		Component = Cast<UObjectPoolTestComponent>(
			Subsystem->GetComponentFromPool(NextOwner, UObjectPoolTestComponent::StaticClass()));
		if (Component != FirstComponent)
		{
			AddError(FString::Printf(TEXT("Component stress did not reuse the original instance at iteration %d"), Index));
			break;
		}
	}
	const double ElapsedMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	AddInfo(FString::Printf(TEXT("5,000 Component release/acquire cycles completed in %.3f ms"), ElapsedMilliseconds));

	if (Subsystem != nullptr && Component != nullptr)
	{
		const FObjectPoolClassStats Stats = Subsystem->GetComponentPoolStats(UObjectPoolTestComponent::StaticClass());
		TestEqual(TEXT("Component stress creates only one instance"), Stats.Created, 1);
		TestEqual(TEXT("Every component reacquire is a pool hit"), Stats.PoolHits, Iterations);
		TestEqual(TEXT("Component acquire callback count matches reuse count"), Component->AcquiredCount, Iterations + 1);
		TestEqual(TEXT("Component release callback count matches iteration count"), Component->ReleasedCount, Iterations);
		TestTrue(TEXT("Subsystem invariants hold after component stress"), Subsystem->Validate());
	}

	ObjectPoolSubsystemTests::DestroyTestWorld(World);
	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolBlueprintInheritedLifecycleTest,
	"Pool.ObjectPool.Inheritance.ActorLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolBlueprintInheritedLifecycleTest::RunTest(const FString& Parameters)
{
	UClass* DerivedClass = AObjectPoolTestDerivedNetworkActor::StaticClass();
	TestTrue(TEXT("Derived fixture inherits the poolable Actor interface"),
		DerivedClass->ImplementsInterface(UPoolableActorInterface::StaticClass()));

	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	Settings->bEnabled = true;
	Settings->DefaultModeTag = FGameplayTag();
	Settings->ModeConfigs.Reset();

	FActorPoolClassConfig ActorConfig;
	ActorConfig.ActorClass = DerivedClass;
	ActorConfig.MaxPoolSize = 1;
	ActorConfig.bAllowRuntimeGrowth = true;
	FPoolModeConfig ModeConfig;
	ModeConfig.ActorPools.Add(ActorConfig);
	Settings->ModeConfigs.Add(FGameplayTag(), ModeConfig);

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolBlueprintLifecycleWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	AActor* First = Subsystem != nullptr
		? Subsystem->SpawnActorFromPool(DerivedClass, FTransform::Identity)
		: nullptr;
	TestNotNull(TEXT("Inherited-interface Actor can be acquired"), First);

	if (Subsystem != nullptr && First != nullptr)
	{
		TestFalse(TEXT("Inherited-interface Actor is visible while active"), First->IsHidden());
		TestTrue(TEXT("Inherited-interface Actor can be released"), Subsystem->ReleaseActorToPool(First));
		TestTrue(TEXT("Inherited release path hides the Actor"), First->IsHidden());
		TestFalse(TEXT("Inherited release path disables collision"), First->GetActorEnableCollision());

		AActor* Reused = Subsystem->SpawnActorFromPool(
			DerivedClass,
			FTransform(FVector(100.0, 0.0, 0.0)));
		TestEqual(TEXT("Inherited-interface Actor instance is reused"), Reused, First);
		TestFalse(TEXT("Inherited acquire path shows the Actor"), Reused->IsHidden());
		TestTrue(TEXT("Inherited acquire path enables collision"), Reused->GetActorEnableCollision());
		TestTrue(TEXT("Subsystem invariants hold after inherited lifecycle"), Subsystem->Validate());
	}

	ObjectPoolSubsystemTests::DestroyTestWorld(World);
	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FObjectPoolRuntimeModeSwitchTest,
	"Pool.ObjectPool.Subsystem.RuntimeModeSwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FObjectPoolRuntimeModeSwitchTest::RunTest(const FString& Parameters)
{
	const FGameplayTag ModeATag = ObjectPoolSubsystemTests::TAG_ObjectPoolTestModeA;
	const FGameplayTag ModeBTag = ObjectPoolSubsystemTests::TAG_ObjectPoolTestModeB;
	UObjectPoolSettings* Settings = GetMutableDefault<UObjectPoolSettings>();
	const bool PreviousEnabled = Settings->bEnabled;
	const FGameplayTag PreviousDefaultMode = Settings->DefaultModeTag;
	const TMap<FGameplayTag, FPoolModeConfig> PreviousModeConfigs = Settings->ModeConfigs;

	FActorPoolClassConfig ModeAActorConfig;
	ModeAActorConfig.ActorClass = AActor::StaticClass();
	ModeAActorConfig.MaxPoolSize = 1;
	FPoolModeConfig ModeAConfig;
	ModeAConfig.ActorPools.Add(ModeAActorConfig);

	FActorPoolClassConfig ModeBActorConfig;
	ModeBActorConfig.ActorClass = AActor::StaticClass();
	ModeBActorConfig.MaxPoolSize = 2;
	FPoolModeConfig ModeBConfig;
	ModeBConfig.ActorPools.Add(ModeBActorConfig);

	Settings->bEnabled = true;
	Settings->DefaultModeTag = ModeATag;
	Settings->ModeConfigs.Reset();
	Settings->ModeConfigs.Add(ModeATag, ModeAConfig);
	Settings->ModeConfigs.Add(ModeBTag, ModeBConfig);

	UWorld* World = ObjectPoolSubsystemTests::CreateTestWorld(TEXT("ObjectPoolRuntimeModeSwitchWorld"));
	UObjectPoolSubsystem* Subsystem = World != nullptr ? World->GetSubsystem<UObjectPoolSubsystem>() : nullptr;
	TestNotNull(TEXT("Mode switch world creates the subsystem"), Subsystem);
	AActor* ActiveActor = Subsystem != nullptr
		? Subsystem->SpawnActorFromPool(AActor::StaticClass(), FTransform::Identity)
		: nullptr;
	TestNotNull(TEXT("Mode A creates an actor"), ActiveActor);

	if (Subsystem != nullptr && ActiveActor != nullptr)
	{
		TestFalse(TEXT("Mode switch is rejected while an entry is active"),
			Subsystem->SwitchModeTag(ModeBTag));
		TestEqual(TEXT("Rejected mode switch keeps mode A"),
			Subsystem->GetCurrentModeTag(), ModeATag);
		TestTrue(TEXT("Active entry can be released before switching"), Subsystem->ReleaseActorToPool(ActiveActor));
		TestTrue(TEXT("Mode switch succeeds when all entries are inactive"),
			Subsystem->SwitchModeTag(ModeBTag));
		TestEqual(TEXT("Current mode changes to mode B"),
			Subsystem->GetCurrentModeTag(), ModeBTag);
		AActor* ModeBActor = Subsystem->SpawnActorFromPool(AActor::StaticClass(), FTransform::Identity);
		TestNotNull(TEXT("Mode B can acquire using its rebuilt configuration"), ModeBActor);
		const FString DebugReport = Subsystem->BuildDebugReport();
		TestTrue(TEXT("Debug report includes all three prewarm states"),
			DebugReport.Contains(TEXT("Prewarm(Actor=")) &&
			DebugReport.Contains(TEXT("UObject=")) &&
			DebugReport.Contains(TEXT("Component=")));
	}

	ObjectPoolSubsystemTests::DestroyTestWorld(World);
	Settings->bEnabled = PreviousEnabled;
	Settings->DefaultModeTag = PreviousDefaultMode;
	Settings->ModeConfigs = PreviousModeConfigs;
	return true;
}

#endif
