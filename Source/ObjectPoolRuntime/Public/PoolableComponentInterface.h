#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "PoolableComponentInterface.generated.h"

UINTERFACE(BlueprintType)
class OBJECTPOOLRUNTIME_API UPoolableComponentInterface : public UInterface
{
	GENERATED_BODY()
};

class OBJECTPOOLRUNTIME_API IPoolableComponentInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnComponentPoolCreated();

	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnComponentAcquired();

	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnComponentReleased();
};
