# ObjectPoolRuntime

> A reusable, world-scoped Unreal Engine 5 object-pool plugin for Actors, UObjects, and ActorComponents.

[![Unreal Engine](https://img.shields.io/badge/Unreal%20Engine-5.7-0E1128?logo=unrealengine)](https://www.unrealengine.com/)
[![Language](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus)](Source/ObjectPoolRuntime)
[![Platform](https://img.shields.io/badge/runtime-game%20thread-087F5B)](Source/ObjectPoolRuntime/README.md)
[![Repository](https://img.shields.io/badge/repository-public-2DA44E?logo=github)](https://github.com/juyi404/ObjectPoolRuntime)

`ObjectPoolRuntime` moves recurring object-creation work out of gameplay spikes and into controlled prewarming and reuse flows. It is implemented as a standalone runtime plugin and does not depend on a particular game mode, sample project, or gameplay framework.

The plugin supports:

- `AActor`
- plain `UObject`
- `UActorComponent`
- per-mode pool configurations through Gameplay Tags
- client, server, and PIE/editor prewarming
- lifecycle callbacks and configurable framework-state recovery
- replicated lifecycle handling and remote-subobject release policies
- pool statistics, invariant validation, automation tests, and network smoke tests

## Documentation

- **[中文对象池源码导读与接入指南](docs/index.html)** — architecture, configuration, prewarming, lifecycle callbacks, GC, replication, integration examples, troubleshooting, and release checks
- **[Runtime contract](Source/ObjectPoolRuntime/README.md)** — concise lifecycle, recovery, network, mode-switching, diagnostics, and test requirements

> GitHub displays the guide as source. Download the repository and open `docs/index.html` in a browser for the full styled, interactive version.

## Why use this plugin?

Repeated `SpawnActor`/`DestroyActor`, `NewObject`/garbage collection, component registration, and network-subobject setup can create costly runtime spikes. This plugin replaces repeated destruction with a controlled lifecycle:

```text
Create or prewarm -> Acquire -> Use -> Release -> Inactive bucket -> Acquire again
```

The framework owns allocation, storage, capacity, ownership transfer, GC reachability, and supported network cleanup. The pooled gameplay class remains responsible for resetting its own gameplay state.

## Feature overview

| Area | Capability |
| --- | --- |
| Scope | One `UObjectPoolSubsystem` per supported `UWorld` |
| Types | Actors, UObjects, and ActorComponents |
| Configuration | `UDeveloperSettings`, default mode, and Gameplay Tag mode maps |
| Capacity | Per-class preallocation, maximum size, and optional runtime growth |
| Recovery | Business callbacks only, framework-state reset, or full framework reset |
| Actor lifecycle | Separate server/client acquire and release callbacks |
| Object ownership | Inactive objects move under a subsystem-owned pool outer |
| Component ownership | Inactive components are unregistered and moved to a component-pool owner |
| Networking | Replicated Actor generations and explicit remote-subobject release policies |
| Observability | Per-class counts, hits, misses, prewarm status, snapshots, and reports |
| Validation | Bucket/managed ownership invariants and optional validation after mutation |

## Requirements

- Unreal Engine 5.7 is declared in `ObjectPoolRuntime.uplugin`.
- All acquire, release, prewarm, and mode-switch operations must run on the game thread.
- The consuming project must supply its own pool-class configuration.
- Replicated pooled UObjects require an authoritative replicated Actor that uses the registered subobject list.

The code may work with nearby UE5 versions, but compatibility should be verified before changing the plugin descriptor's `EngineVersion`.

## Installation

### Option 1: clone directly into a project

```bash
cd YourProject/Plugins
git clone https://github.com/juyi404/ObjectPoolRuntime.git
```

Your project should then contain:

```text
YourProject/
└── Plugins/
    └── ObjectPoolRuntime/
        ├── ObjectPoolRuntime.uplugin
        ├── Source/
        └── docs/
```

### Option 2: copy a downloaded release or repository

Copy the repository directory to `YourProject/Plugins/ObjectPoolRuntime`.

After installation:

1. Regenerate the Unreal project files.
2. Build the Editor target.
3. Open the project and confirm **Object Pool Runtime** is enabled.
4. Open **Project Settings > Object Pool** and add pool classes.

## Quick start

### 1. Configure a class

Use **Project Settings > Object Pool** or add project-owned values under:

```ini
[/Script/ObjectPoolRuntime.ObjectPoolSettings]
bEnabled=True
```

`DefaultModeConfig` can be used without Gameplay Tags. Larger projects can populate `ModeConfigs` and return the current tag from the configured GameState provider function.

Each class configuration includes:

- `bEnabled`
- `EditorPreallocateCount`
- `ServerPreallocateCount`
- `ClientPreallocateCount`
- `MaxPoolSize`
- `bAllowRuntimeGrowth`
- `RecoveryPolicy`

Object and component configurations can additionally enable replication and select a remote release policy.

### 2. Implement lifecycle callbacks

Actors implement `IPoolableActorInterface`:

```cpp
void AMyProjectile::OnPoolAcquireServer_Implementation(
    const FActorPoolAcquireContext& Context)
{
    SetActorTransform(Context.Transform);
    SetOwner(Context.Owner);
    SetInstigator(Context.Instigator);
    SetActorHiddenInGame(false);
    SetActorEnableCollision(true);
    SetActorTickEnabled(true);
}

void AMyProjectile::OnPoolReleaseServer_Implementation()
{
    GetWorldTimerManager().ClearAllTimersForObject(this);
    SetActorEnableCollision(false);
    SetActorTickEnabled(false);
    SetActorHiddenInGame(true);
    SetOwner(nullptr);
    SetInstigator(nullptr);
}
```

UObjects implement `IPoolableObjectInterface`; components implement `IPoolableComponentInterface`. Creation callbacks run once per instance. Acquire and release callbacks must be safe to run repeatedly.

### 3. Acquire and release

```cpp
#include "ObjectPoolSubsystem.h"

UObjectPoolSubsystem* Pool = GetWorld()->GetSubsystem<UObjectPoolSubsystem>();

AActor* Actor = Pool->SpawnActorFromPool(
    ActorClass,
    SpawnTransform,
    Owner,
    Instigator);

UObject* Object = Pool->GetObjectFromPool(NewOuter, ObjectClass);
UActorComponent* Component =
    Pool->GetComponentFromPool(NewOwner, ComponentClass);

Pool->ReleaseActorToPool(Actor);
Pool->ReleaseObjectToPool(Object);
Pool->ReleaseComponentToPool(Component);
```

Callers must handle a null acquire result when a bucket is empty and runtime growth is disabled or creation fails.

## Lifecycle contract

The managed entry state is:

```text
Unmanaged -> Preallocating -> Inactive -> Active -> Releasing -> Inactive
```

- `Preallocating` and `Releasing` are protected transition states.
- Recursive release is rejected.
- A lifecycle callback is allowed to invalidate its object; the subsystem revalidates ownership and state before continuing.
- A release callback must clear arbitrary gameplay state such as delegates, targets, effects, abilities, materials, physics velocity, Blueprint variables, and gameplay references.
- A recovery policy only controls framework-owned cleanup. It cannot understand every gameplay class.

## Recovery policies

| Policy | Framework behavior | Gameplay responsibility |
| --- | --- | --- |
| `BusinessCallbacksOnly` | Invokes business callbacks; mandatory ownership-transfer cleanup still runs | Highest |
| `ResetFrameworkState` | Also clears timers and resets framework-owned ticking, activation, and registration state | Normal |
| `Full` | Also clears latent actions and additional framework-owned attachment state | Still required for arbitrary gameplay state |

Start with `ResetFrameworkState`, validate callback completeness, and only then tune cleanup strength for performance.

## Runtime modes

The subsystem resolves a mode in this order:

1. configured GameState provider function
2. `DefaultModeTag`
3. `DefaultModeConfig`

Use `SwitchModeTag` only when no entry is active or transitioning. `RefreshModeTag` asks the provider for the current tag again. A successful switch rebuilds and prewarms the pools for the new configuration.

## Network replication

Pooled Actors replicate one lifecycle state containing:

- `Active`
- `Generation`
- `AcquireContext`

Generation changes allow a client to reconstruct a release callback when a fast release/acquire cycle is collapsed into one replication update.

Replicated UObject and component release policies have different reuse guarantees:

| Policy | Existing client replica | Reuse same server instance? |
| --- | --- | --- |
| `DestroyRemoteReplica` | Explicitly removed | Yes |
| `UnregisterOnly` | May remain temporarily | No; the server instance is discarded |
| `TearOffRemoteReplica` | Becomes independent | No; the server instance is discarded |

Only `DestroyRemoteReplica` provides traditional same-instance pool reuse for replicated server UObjects and components.

## Diagnostics

| Command | Purpose |
| --- | --- |
| `Pool.Dump` | Print active/inactive counts, capacity, creation, prewarm, hits, misses, and hit rate |
| `Pool.Validate` | Check Bucket-to-Managed and Managed-to-Bucket ownership invariants |
| `Pool.ValidateOnMutation 1` | Validate after every acquire and release while diagnosing integration issues |
| `Pool.NetSmoke` | Run Actor reuse and UObject/component cross-owner network smoke coverage |
| `Pool.NetSmoke.ReleasePolicy 0\|1\|2` | Select UnregisterOnly, DestroyRemoteReplica, or TearOffRemoteReplica |
| `Pool.NetSmoke.StartDelay N` | Delay smoke execution to allow a packaged client to connect |

## Testing

The repository includes subsystem, configuration, container, lifecycle, mutation, and network-smoke coverage under `Source/ObjectPoolRuntime/Private/Tests`.

When the plugin is used inside the companion sample project, the following scripts build and exercise it:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Scripts\TestObjectPool.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Scripts\BuildObjectPool.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Scripts\NetworkSmokeObjectPool.ps1 \
  -PackageDirectory .\Saved\PackagedTests\ObjectPoolGenericComplete
```

Those scripts belong to the sample project rather than this standalone plugin repository.

## Repository layout

```text
ObjectPoolRuntime/
├── ObjectPoolRuntime.uplugin
├── Source/ObjectPoolRuntime/
│   ├── Public/                  # Public API, settings, types, and interfaces
│   ├── Private/                 # Subsystem, buckets, replication, and module code
│   ├── Private/Tests/           # Automation and network test fixtures
│   ├── ObjectPoolRuntime.Build.cs
│   └── README.md                # Detailed runtime contract
└── docs/
    ├── index.html               # Chinese interactive guide
    └── object-pool-article-assets/
```

## Integration checklist

- [ ] Pool classes are configured for the correct default or tagged mode.
- [ ] Preallocation counts do not exceed `MaxPoolSize`.
- [ ] Acquire and release callbacks reset all gameplay-owned state.
- [ ] Components can move across owners without retaining old bindings.
- [ ] Replicated subobjects use the intended remote release policy.
- [ ] Null acquire results have a caller-side fallback.
- [ ] `Pool.Validate` passes after representative gameplay.
- [ ] Pool hit rate and memory use are reviewed with `Pool.Dump`.
- [ ] PIE restart, map unload, and mode switching leave no active entries behind.
- [ ] Network behavior is tested with all release policies used by the project.

## Contributing

Issues and focused pull requests are welcome. Include the Unreal Engine version, reproduction steps, relevant pool configuration, expected lifecycle sequence, and output from `Pool.Dump` or `Pool.Validate` when reporting a runtime problem.

## License

No license has been granted yet. Public visibility does not by itself grant permission to copy, modify, redistribute, or incorporate this code. Add a `LICENSE` file before inviting external reuse or contributions under defined terms.
