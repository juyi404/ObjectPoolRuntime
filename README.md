# ObjectPoolRuntime

`ObjectPoolRuntime` is a reusable Unreal Engine 5 runtime plugin that provides game-thread object pools for Actors, UObjects, and ActorComponents.

## Features

- World-scoped pools implemented with `UWorldSubsystem`
- Actor, UObject, and ActorComponent pooling
- Per-mode configuration through Gameplay Tags
- Editor, server, and client preallocation counts
- Configurable recovery policies and runtime growth limits
- Server/client lifecycle callbacks for pooled Actors
- Replicated UObject and component ownership-transfer policies
- Pool statistics, validation commands, automation tests, and network smoke coverage

## Installation

1. Copy this repository into your project's `Plugins/ObjectPoolRuntime` directory.
2. Regenerate the Unreal project files.
3. Build the project and enable **Object Pool Runtime** if it is not enabled automatically.
4. Configure pool classes under `[/Script/ObjectPoolRuntime.ObjectPoolSettings]` or in **Project Settings > Object Pool**.

The current plugin descriptor targets Unreal Engine 5.7. Update `EngineVersion` in `ObjectPoolRuntime.uplugin` when using another compatible engine version.

## Runtime contract

- Acquire, release, prewarm, and mode-switch operations are game-thread only.
- Business state must be reset by the pool lifecycle callbacks implemented by each pooled type.
- Replicated pooled UObjects require an authoritative replicated Actor that uses the registered subobject list.
- Only `DestroyRemoteReplica` permits the same replicated server UObject or component instance to return to the pool for later reuse.

See [Source/ObjectPoolRuntime/README.md](Source/ObjectPoolRuntime/README.md) for the detailed lifecycle, recovery, network, diagnostics, and testing contract.

## Main API

- `UObjectPoolSubsystem::SpawnActorFromPool`
- `UObjectPoolSubsystem::ReleaseActorToPool`
- `UObjectPoolSubsystem::GetObjectFromPool`
- `UObjectPoolSubsystem::ReleaseObjectToPool`
- `UObjectPoolSubsystem::GetComponentFromPool`
- `UObjectPoolSubsystem::ReleaseComponentToPool`

## Diagnostics

- `Pool.Dump`
- `Pool.Validate`
- `Pool.ValidateOnMutation 1`
- `Pool.NetSmoke`

## License

No license has been granted yet. Add a license file before accepting external reuse or contributions.
