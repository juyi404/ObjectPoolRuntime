#if WITH_DEV_AUTOMATION_TESTS

#include "PooledObjectBucket.h"
#include "ObjectPoolTestObject.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPooledObjectBucketLifecycleTest,
	"Pool.ObjectPool.Bucket.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPooledObjectBucketLifecycleTest::RunTest(const FString& Parameters)
{
	FPooledObjectBucket Bucket;
	Bucket.Configure(2);

	UObject* First = NewObject<UObjectPoolTestObject>(GetTransientPackage());
	UObject* Second = NewObject<UObjectPoolTestObject>(GetTransientPackage());

	TestEqual(TEXT("Register first"), Bucket.RegisterActive(First), EObjectPoolOperationResult::Succeeded);
	TestEqual(TEXT("Reject duplicate active registration"), Bucket.RegisterActive(First), EObjectPoolOperationResult::AlreadyActive);
	TestEqual(TEXT("Release first"), Bucket.Release(First), EObjectPoolOperationResult::Succeeded);
	TestEqual(TEXT("Reject duplicate release"), Bucket.Release(First), EObjectPoolOperationResult::AlreadyInactive);
	TestEqual(TEXT("First is inactive"), Bucket.GetState(First), EObjectPoolEntryState::Inactive);

	TestEqual(TEXT("Register second"), Bucket.RegisterActive(Second), EObjectPoolOperationResult::Succeeded);
	TestEqual(TEXT("Release second"), Bucket.Release(Second), EObjectPoolOperationResult::Succeeded);
	TestEqual(TEXT("Inactive count"), Bucket.NumInactive(), 2);

	TestEqual(TEXT("Acquire is LIFO"), Bucket.Acquire(), Second);
	TestEqual(TEXT("Acquired object is active"), Bucket.GetState(Second), EObjectPoolEntryState::Active);
	TestEqual(TEXT("Acquire remaining"), Bucket.Acquire(), First);
	TestNull(TEXT("Empty bucket returns null"), Bucket.Acquire());

	FString ValidationError;
	TestTrue(TEXT("Bucket invariants hold"), Bucket.Validate(&ValidationError));
	if (!ValidationError.IsEmpty())
	{
		AddError(ValidationError);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPooledObjectBucketCapacityTest,
	"Pool.ObjectPool.Bucket.Capacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPooledObjectBucketCapacityTest::RunTest(const FString& Parameters)
{
	FPooledObjectBucket Bucket;
	Bucket.Configure(1);

	UObject* First = NewObject<UObjectPoolTestObject>(GetTransientPackage());
	UObject* Second = NewObject<UObjectPoolTestObject>(GetTransientPackage());
	Bucket.RegisterActive(First);
	Bucket.RegisterActive(Second);

	TestEqual(TEXT("First release fits"), Bucket.Release(First), EObjectPoolOperationResult::Succeeded);
	TestEqual(TEXT("Second release reaches capacity"), Bucket.Release(Second), EObjectPoolOperationResult::CapacityReached);
	TestTrue(TEXT("Rejected object remains active for caller policy"), Bucket.ContainsActive(Second));
	TestTrue(TEXT("Bucket remains valid"), Bucket.Validate());

	return true;
}

#endif
