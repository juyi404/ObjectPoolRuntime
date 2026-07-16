#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogObjectPool, Log, All);

OBJECTPOOLRUNTIME_API bool IsObjectPoolRuntimeEnabled();
OBJECTPOOLRUNTIME_API bool IsObjectPoolDebugLoggingEnabled();
OBJECTPOOLRUNTIME_API bool IsObjectPoolMutationValidationEnabled();
OBJECTPOOLRUNTIME_API bool IsObjectPoolNetworkSupportEnabled();
