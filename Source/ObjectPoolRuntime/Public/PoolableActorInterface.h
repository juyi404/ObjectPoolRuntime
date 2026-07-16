#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ObjectPoolTypes.h"
#include "PoolableActorInterface.generated.h"

UINTERFACE(BlueprintType)
class OBJECTPOOLRUNTIME_API UPoolableActorInterface : public UInterface
{
	GENERATED_BODY()
};

class OBJECTPOOLRUNTIME_API IPoolableActorInterface
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnPoolCreated();

	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnPoolAcquireServer(const FActorPoolAcquireContext& Context);

	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnPoolAcquireClient(const FActorPoolAcquireContext& Context);

	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnPoolReleaseServer();

	UFUNCTION(BlueprintNativeEvent, Category = "Object Pool")
	void OnPoolReleaseClient();
};
