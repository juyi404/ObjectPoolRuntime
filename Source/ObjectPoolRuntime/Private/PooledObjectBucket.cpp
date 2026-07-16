#include "PooledObjectBucket.h"

void FPooledObjectBucket::Configure(const int32 InMaxPoolSize)
{
	MaxPoolSize = FMath::Max(1, InMaxPoolSize);
}

EObjectPoolOperationResult FPooledObjectBucket::RegisterActive(UObject* Object)
{
	if (!IsValid(Object))
	{
		return EObjectPoolOperationResult::InvalidObject;
	}

	if (ActiveObjects.Contains(Object))
	{
		return EObjectPoolOperationResult::AlreadyActive;
	}

	if (ContainsInactive(Object))
	{
		return EObjectPoolOperationResult::AlreadyInactive;
	}

	ActiveObjects.Add(Object);
	return EObjectPoolOperationResult::Succeeded;
}

EObjectPoolOperationResult FPooledObjectBucket::Release(UObject* Object)
{
	if (!IsValid(Object))
	{
		return EObjectPoolOperationResult::InvalidObject;
	}

	if (ContainsInactive(Object))
	{
		return EObjectPoolOperationResult::AlreadyInactive;
	}

	if (!ActiveObjects.Contains(Object))
	{
		return EObjectPoolOperationResult::NotActive;
	}

	if (InactiveObjects.Num() >= MaxPoolSize)
	{
		return EObjectPoolOperationResult::CapacityReached;
	}

	ActiveObjects.Remove(Object);
	InactiveObjects.Add(Object);
	return EObjectPoolOperationResult::Succeeded;
}

UObject* FPooledObjectBucket::Acquire()
{
	while (!InactiveObjects.IsEmpty())
	{
		UObject* Object = InactiveObjects.Pop(EAllowShrinking::No);
		if (!IsValid(Object))
		{
			continue;
		}

		if (ActiveObjects.Contains(Object))
		{
			continue;
		}

		ActiveObjects.Add(Object);
		return Object;
	}

	return nullptr;
}

bool FPooledObjectBucket::Remove(UObject* Object)
{
	if (Object == nullptr)
	{
		return false;
	}

	const int32 RemovedInactive = InactiveObjects.Remove(Object);
	const int32 RemovedActive = ActiveObjects.Remove(Object);
	return RemovedInactive > 0 || RemovedActive > 0;
}

int32 FPooledObjectBucket::RemoveInvalidObjects()
{
	const int32 InitialCount = NumTotal();
	InactiveObjects.RemoveAll([](const TObjectPtr<UObject>& Object)
	{
		return !IsValid(Object);
	});

	for (auto It = ActiveObjects.CreateIterator(); It; ++It)
	{
		if (!IsValid(*It))
		{
			It.RemoveCurrent();
		}
	}

	return InitialCount - NumTotal();
}

void FPooledObjectBucket::Reset()
{
	InactiveObjects.Reset();
	ActiveObjects.Reset();
}

void FPooledObjectBucket::GetObjects(TArray<UObject*>& OutObjects) const
{
	OutObjects.Reserve(OutObjects.Num() + NumTotal());
	for (const TObjectPtr<UObject>& Object : ActiveObjects)
	{
		OutObjects.Add(Object.Get());
	}
	for (const TObjectPtr<UObject>& Object : InactiveObjects)
	{
		OutObjects.Add(Object.Get());
	}
}

bool FPooledObjectBucket::ContainsActive(const UObject* Object) const
{
	return Object != nullptr && ActiveObjects.Contains(Object);
}

bool FPooledObjectBucket::ContainsInactive(const UObject* Object) const
{
	return Object != nullptr && InactiveObjects.Contains(Object);
}

EObjectPoolEntryState FPooledObjectBucket::GetState(const UObject* Object) const
{
	if (ContainsActive(Object))
	{
		return EObjectPoolEntryState::Active;
	}

	if (ContainsInactive(Object))
	{
		return EObjectPoolEntryState::Inactive;
	}

	return EObjectPoolEntryState::Unmanaged;
}

bool FPooledObjectBucket::Validate(FString* OutError) const
{
	auto Fail = [OutError](const TCHAR* Message)
	{
		if (OutError != nullptr)
		{
			*OutError = Message;
		}
		return false;
	};

	if (MaxPoolSize < 1)
	{
		return Fail(TEXT("MaxPoolSize must be at least one."));
	}

	if (InactiveObjects.Num() > MaxPoolSize)
	{
		return Fail(TEXT("Inactive object count exceeds MaxPoolSize."));
	}

	TSet<const UObject*> SeenInactive;
	for (const TObjectPtr<UObject>& Object : InactiveObjects)
	{
		if (!IsValid(Object))
		{
			return Fail(TEXT("Inactive objects contain an invalid entry."));
		}

		if (SeenInactive.Contains(Object.Get()))
		{
			return Fail(TEXT("Inactive objects contain a duplicate entry."));
		}

		if (ActiveObjects.Contains(Object))
		{
			return Fail(TEXT("An object exists in both active and inactive collections."));
		}

		SeenInactive.Add(Object.Get());
	}

	for (const TObjectPtr<UObject>& Object : ActiveObjects)
	{
		if (!IsValid(Object))
		{
			return Fail(TEXT("Active objects contain an invalid entry."));
		}
	}

	if (OutError != nullptr)
	{
		OutError->Reset();
	}
	return true;
}
