#include "ObjectPoolNetworkSmokeActor.h"

#include "ObjectPoolRuntime.h"
#include "Net/UnrealNetwork.h"
#include "PooledActorNetworkStateComponent.h"

void UObjectPoolNetworkSmokeObject::OnObjectAcquired_Implementation()
{
	++AcquireSerial;
	ServerOwnerName = GetNameSafe(GetTypedOuter<AActor>());
	UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_SERVER_OBJECT_ACQUIRE object=%s serial=%d owner=%s"),
		*GetName(), AcquireSerial, *ServerOwnerName);
}

void UObjectPoolNetworkSmokeObject::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UObjectPoolNetworkSmokeObject, AcquireSerial);
	DOREPLIFETIME(UObjectPoolNetworkSmokeObject, ServerOwnerName);
}

void UObjectPoolNetworkSmokeObject::OnRep_AcquireSerial()
{
	UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_CLIENT_OBJECT_ACQUIRE object=%s serial=%d server_owner=%s local_outer=%s"),
		*GetName(), AcquireSerial, *ServerOwnerName, *GetNameSafe(GetOuter()));
}

UObjectPoolNetworkSmokeComponent::UObjectPoolNetworkSmokeComponent()
{
	SetIsReplicatedByDefault(true);
}

void UObjectPoolNetworkSmokeComponent::OnComponentAcquired_Implementation()
{
	++AcquireSerial;
	ServerOwnerName = GetNameSafe(GetOwner());
	UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_SERVER_COMPONENT_ACQUIRE component=%s serial=%d owner=%s"),
		*GetName(), AcquireSerial, *ServerOwnerName);
}

void UObjectPoolNetworkSmokeComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UObjectPoolNetworkSmokeComponent, AcquireSerial);
	DOREPLIFETIME(UObjectPoolNetworkSmokeComponent, ServerOwnerName);
}

void UObjectPoolNetworkSmokeComponent::OnRep_AcquireSerial()
{
	UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_CLIENT_COMPONENT_ACQUIRE component=%s serial=%d server_owner=%s local_owner=%s"),
		*GetName(), AcquireSerial, *ServerOwnerName, *GetNameSafe(GetOwner()));
}

AObjectPoolNetworkSmokeActor::AObjectPoolNetworkSmokeActor()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	bReplicateUsingRegisteredSubObjectList = true;
	PrimaryActorTick.bCanEverTick = false;
	NetworkState = CreateDefaultSubobject<UPooledActorNetworkStateComponent>(TEXT("PoolNetworkState"));
}

void AObjectPoolNetworkSmokeActor::OnPoolAcquireServer_Implementation(const FActorPoolAcquireContext& Context)
{
	UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_SERVER_ACQUIRE actor=%s"), *GetName());
}

void AObjectPoolNetworkSmokeActor::OnPoolAcquireClient_Implementation(const FActorPoolAcquireContext& Context)
{
	UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_CLIENT_ACQUIRE actor=%s"), *GetName());
}

void AObjectPoolNetworkSmokeActor::OnPoolReleaseServer_Implementation()
{
	UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_SERVER_RELEASE actor=%s"), *GetName());
}

void AObjectPoolNetworkSmokeActor::OnPoolReleaseClient_Implementation()
{
	UE_LOG(LogObjectPool, Display, TEXT("POOL_NET_CLIENT_RELEASE actor=%s"), *GetName());
}
