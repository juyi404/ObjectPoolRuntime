#include "ObjectPoolSettings.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"

UObjectPoolSettings::UObjectPoolSettings()
{
	CategoryName = TEXT("Game");
	SectionName = TEXT("ObjectPool");
}

FName UObjectPoolSettings::GetCategoryName() const
{
	return TEXT("Game");
}

bool UObjectPoolSettings::ValidateSettings(FString* OutError) const
{
	auto Fail = [OutError](const FString& Message)
	{
		if (OutError != nullptr)
		{
			*OutError = Message;
		}
		return false;
	};

	auto ValidateBase = [&Fail](const FPoolClassConfigBase& Config, const FString& Label)
	{
		if (Config.EditorPreallocateCount < 0 || Config.ServerPreallocateCount < 0 || Config.ClientPreallocateCount < 0)
		{
			return Fail(FString::Printf(TEXT("%s has a negative preallocate count."), *Label));
		}
		if (Config.MaxPoolSize < 1)
		{
			return Fail(FString::Printf(TEXT("%s has MaxPoolSize < 1."), *Label));
		}
		if (Config.EditorPreallocateCount > Config.MaxPoolSize ||
			Config.ServerPreallocateCount > Config.MaxPoolSize ||
			Config.ClientPreallocateCount > Config.MaxPoolSize)
		{
			return Fail(FString::Printf(TEXT("%s preallocate count exceeds MaxPoolSize."), *Label));
		}
		return true;
	};

	auto ValidateMode = [&Fail, &ValidateBase](const FPoolModeConfig& Mode, const FString& ModeLabel)
	{
		TSet<FSoftObjectPath> ActorClasses;
		for (int32 Index = 0; Index < Mode.ActorPools.Num(); ++Index)
		{
			const FActorPoolClassConfig& Config = Mode.ActorPools[Index];
			const FString Label = FString::Printf(TEXT("%s ActorPools[%d]"), *ModeLabel, Index);
			if (!ValidateBase(Config, Label))
			{
				return false;
			}
			const FSoftObjectPath Path = Config.ActorClass.ToSoftObjectPath();
			if (Path.IsNull())
			{
				return Fail(FString::Printf(TEXT("%s has an empty ActorClass."), *Label));
			}
			if (ActorClasses.Contains(Path))
			{
				return Fail(FString::Printf(TEXT("%s duplicates ActorClass %s."), *Label, *Path.ToString()));
			}
			ActorClasses.Add(Path);
			if (UClass* Class = Config.ActorClass.Get())
			{
				if (!Class->IsChildOf(AActor::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				{
					return Fail(FString::Printf(TEXT("%s references an invalid Actor class %s."), *Label, *GetNameSafe(Class)));
				}
			}
		}

		TSet<FSoftObjectPath> ObjectClasses;
		for (int32 Index = 0; Index < Mode.ObjectPools.Num(); ++Index)
		{
			const FObjectPoolClassConfig& Config = Mode.ObjectPools[Index];
			const FString Label = FString::Printf(TEXT("%s ObjectPools[%d]"), *ModeLabel, Index);
			if (!ValidateBase(Config, Label))
			{
				return false;
			}
			const FSoftObjectPath Path = Config.ObjectClass;
			if (Path.IsNull())
			{
				return Fail(FString::Printf(TEXT("%s has an empty ObjectClass."), *Label));
			}
			if (ObjectClasses.Contains(Path))
			{
				return Fail(FString::Printf(TEXT("%s duplicates ObjectClass %s."), *Label, *Path.ToString()));
			}
			ObjectClasses.Add(Path);
			if (UClass* Class = Config.ObjectClass.ResolveClass())
			{
				if (Class->IsChildOf(AActor::StaticClass()) || Class->IsChildOf(UActorComponent::StaticClass()) ||
					Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				{
					return Fail(FString::Printf(TEXT("%s references an invalid UObject class %s."), *Label, *GetNameSafe(Class)));
				}
			}
		}

		TSet<FSoftObjectPath> ComponentClasses;
		for (int32 Index = 0; Index < Mode.ComponentPools.Num(); ++Index)
		{
			const FComponentPoolClassConfig& Config = Mode.ComponentPools[Index];
			const FString Label = FString::Printf(TEXT("%s ComponentPools[%d]"), *ModeLabel, Index);
			if (!ValidateBase(Config, Label))
			{
				return false;
			}
			const FSoftObjectPath Path = Config.ComponentClass;
			if (Path.IsNull())
			{
				return Fail(FString::Printf(TEXT("%s has an empty ComponentClass."), *Label));
			}
			if (ComponentClasses.Contains(Path))
			{
				return Fail(FString::Printf(TEXT("%s duplicates ComponentClass %s."), *Label, *Path.ToString()));
			}
			ComponentClasses.Add(Path);
			if (UClass* Class = Config.ComponentClass.ResolveClass())
			{
				if (!Class->IsChildOf(UActorComponent::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				{
					return Fail(FString::Printf(TEXT("%s references an invalid Component class %s."), *Label, *GetNameSafe(Class)));
				}
			}
		}
		return true;
	};

	if (!ValidateMode(DefaultModeConfig, TEXT("DefaultModeConfig")))
	{
		return false;
	}
	for (const TPair<FGameplayTag, FPoolModeConfig>& Pair : ModeConfigs)
	{
		if (!ValidateMode(Pair.Value, FString::Printf(TEXT("ModeConfigs[%s]"), *Pair.Key.ToString())))
		{
			return false;
		}
	}

	if (OutError != nullptr)
	{
		OutError->Reset();
	}
	return true;
}
