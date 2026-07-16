#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ObjectPoolContainer.generated.h"

/** Concrete transient owner used to keep inactive pooled UObjects isolated. */
UCLASS(NotBlueprintable, Transient)
class OBJECTPOOLRUNTIME_API UObjectPoolContainer final : public UObject
{
	GENERATED_BODY()
};
