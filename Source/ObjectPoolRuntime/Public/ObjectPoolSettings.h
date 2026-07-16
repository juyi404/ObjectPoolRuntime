#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ObjectPoolTypes.h"
#include "ObjectPoolSettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Object Pool"))
class OBJECTPOOLRUNTIME_API UObjectPoolSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UObjectPoolSettings();

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Modes")
	FGameplayTag DefaultModeTag;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Modes")
	FName ModeTagProviderFunctionName = TEXT("GetObjectPoolModeTag");

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Modes", meta = (ForceInlineRow))
	TMap<FGameplayTag, FPoolModeConfig> ModeConfigs;

	/** Used when no matching tagged mode exists. Also keeps simple projects tag-free. */
	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Modes")
	FPoolModeConfig DefaultModeConfig;

	UPROPERTY(EditAnywhere, Config, BlueprintReadOnly, Category = "Runtime")
	bool bEnabled = true;

	virtual FName GetCategoryName() const override;

	/** Validates all default and tagged pool configurations without creating pool instances. */
	bool ValidateSettings(FString* OutError = nullptr) const;
};
