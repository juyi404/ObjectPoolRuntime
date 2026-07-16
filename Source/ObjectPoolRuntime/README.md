# ObjectPoolRuntime

`ObjectPoolRuntime` is a game-thread-only, project-agnostic pool for Actors, UObjects and ActorComponents. The module has no dependency on the sample Shooter/Horror gameplay variants.

## Lifecycle contract

- Acquire and release APIs must be called on the game thread.
- `Preallocating` and `Releasing` are protected transition states. Recursive release is rejected.
- A lifecycle callback may invalidate its object. The subsystem revalidates managed ownership and bucket state before continuing.
- An inactive UObject is moved under the subsystem-owned pool outer. An inactive component is unregistered and moved under the component pool owner so it can later be assigned to another Actor safely.
- Gameplay state must be reset by `OnPoolRelease*`/`OnPoolAcquire*`, `OnObjectReleased`/`OnObjectAcquired`, or `OnComponentReleased`/`OnComponentAcquired` callbacks.

## Recovery policies

- `BusinessCallbacksOnly`: invokes business callbacks only. Mandatory component detach/unregister and ownership transfer still occur.
- `ResetFrameworkState`: also clears timers and resets framework-owned ticking/activation state.
- `Full`: additionally clears latent actions and other framework-owned attachment state. It does **not** reset arbitrary Blueprint variables, dynamic delegates, materials, Niagara/audio state, physics velocity, abilities, buffs or gameplay references.

## Network contract

- Pooled Actors use one replicated lifecycle state containing Active, Generation and AcquireContext. A generation change reconstructs a Release callback if Release and Acquire were collapsed into one replication update.
- Replicated pooled UObjects require an authoritative replicated Actor using the registered subobject list.
- Replicated pooled components are unregistered from their previous Actor before ownership transfer.
- `DestroyRemoteReplica` is the only policy that returns the same replicated server UObject/Component instance to the pool for later reuse.
- `UnregisterOnly` stops future replication but intentionally does not guarantee immediate removal of an existing client replica. To prevent network-identity/Owner conflicts, the released server instance is discarded rather than reused.
- `TearOffRemoteReplica` intentionally leaves an independent client replica. The released server instance is discarded because a torn-off network identity cannot safely be reused.

## Runtime modes

Call `SwitchModeTag` only when no pool entry is active or transitioning. `RefreshModeTag` resolves the current tag again. Projects that do not provide a mode tag safely use `DefaultModeConfig`; this module does not require a gameplay-specific mode implementation.

## Diagnostics and tests

- `Pool.Dump` prints pool counts, hit rates and prewarm status.
- `Pool.Validate` checks Bucket-to-Managed and Managed-to-Bucket ownership invariants.
- `Pool.ValidateOnMutation 1` validates after every acquire/release while diagnosing integration problems.
- `Pool.NetSmoke` runs Actor reuse plus UObject/Component cross-owner reuse in an authority world.
- `Pool.NetSmoke.ReleasePolicy 0|1|2` selects UnregisterOnly, DestroyRemoteReplica or TearOffRemoteReplica for the smoke run.
- `Pool.NetSmoke.StartDelay N` gives a packaged client time to join before the first acquire.

Use `Scripts/TestObjectPool.ps1` from the project root to build and run the editor automation suite. Add `-SkipBuild` when the binaries are already current.
