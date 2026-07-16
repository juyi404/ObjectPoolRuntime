#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "PoolableObjectInterface.generated.h"

UINTERFACE(BlueprintType)
class OBJECTPOOLRUNTIME_API UPoolableObjectInterface : public UInterface
{
	GENERATED_BODY()
};

class OBJECTPOOLRUNTIME_API IPoolableObjectInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnObjectPoolCreated();

	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnObjectAcquired();

	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnObjectReleased();
};
