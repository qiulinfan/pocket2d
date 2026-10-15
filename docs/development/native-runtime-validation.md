# Native runtime input validation

Phase 9 covers direct Lua Rigidbody controls/configuration and ParticleSystem
numeric properties/emission. It builds on scene/host-spec preflight but also
checks values supplied to an already-live native object. It does not certify all
native APIs, Transform/SpriteRenderer fields, arbitrary Box2D configurations or
aggregate native memory usage.

## Rigidbody boundary

Rigidbody configuration numeric setters require actual numeric values rather
than numeric strings; boolean flags and body/shape enums enforce their declared
input types. Engine-owned identity fields are outside this validation claim.
Unsupported values are rejected before changing the stored configuration.
Rigidbody conversion checks finite float range and rejects a nonzero double that
would narrow to float zero; ordinary float rounding is still possible. Position
uses the existing ±1e8 range, dimensions use 0.001..1e6, material/damping values
must be nonnegative with arithmetic headroom, and body/shape names use the
supported enumerations.

Gravity validation checks derived acceleration and squared-magnitude headroom,
not merely whether `gravity_scale` itself is finite. It also checks the bundled
Box2D integrator's actual intermediate products, including `gravityScale * mass`
and the resulting gravity force, before combining applied forces and predicting
velocity. An algebraically finite simplified acceleration does not make an
overflowing intermediate multiplication safe.

Mass and inertia checks cover both upper bounds and the reciprocals of tiny
positive values. Shape mass calculations, combined mass/inertia and centered
inertia must retain safe float ranges; positivity alone does not make a very
small mass usable. Motion checks also bound the body's own linear/angular
momentum products, material multipliers and geometry lever arms before solver
use. These guards do not prove stability for every heterogeneous contact or
joint system.

Methods validate vectors and scalar controls before passing them to Box2D.
Force validation accounts for the current body and accumulated force in the
current step; a large force is not rejected solely by a universal force-number
cutoff. Zero direction retains its established no-op behavior. Large finite
angles are not categorically invalid; conversion and derived physics bounds
determine whether a value is supported.

Raw fields remain author configuration. A valid raw density, geometry or damping
assignment does not promise to rebuild an existing Box2D fixture immediately.
Cross-field mass/inertia and derived checks run when configuration is consumed.
The per-frame hierarchy synchronization has a same-type fast path: synchronizing
an already-effective body type does not consume pending raw configuration or
revalidate it as if fixtures were being rebuilt. A real body-type change validates
the actual live fixtures, their mass/inertia, pose and motion before retyping;
it does not substitute pending author geometry for the fixtures that exist.
`SetPosition`, `SetVelocity`, `SetRotation`, gravity and other explicit methods
remain the live control path. Normal invalid-input failures preserve prior values
and native references; this does not promise rollback of arbitrary allocation or
host failures after unrelated operations have already changed gameplay.

## Particle boundary

Numeric properties reject wrong types, fractional integer fields, values outside
int range, NaN/Infinity and float overflow before narrowing. The exception is
`end_scale`: NaN remains its documented unset sentinel and appears as JSON `null`
in the agent's ParticleSystem observation. Duration, interval and burst quantity
are integers in 1..1,000,000.

Particle float setters promise the stated type, finite-value and overflow checks.
They do not provide the Rigidbody nonzero-to-zero underflow rejection or a general
guarantee of exact double-to-float precision.

Scalar endpoint setters allow configuration in either order. For example, setting
a new minimum before its maximum can temporarily reverse a range. Initialization
and Burst validate the coherent configuration, distribution ranges, derived
lifetime/position behavior and reserve capacity before emission. Finish a valid
configuration before the next operation that consumes it. An automatic emission
whose complete wave would exceed the live budget skips that whole wave; a manual
Burst rejects instead. The per-emitter live/reserve cap is one million particles,
not a process-wide memory sandbox.

Existing color behavior is preserved: start color integers clamp to 0..255 at
emission, and end-channel integers outside 0..255 disable interpolation for that
channel. Those integers are not rejected merely for being outside byte range;
noninteger, nonfinite or out-of-int values still fail. Initialization-time
configuration/distribution snapshots also remain: raw field edits are not a
blanket promise to reconfigure every property of an existing emitter immediately.
Origin, color, image, sorting and burst cadence retain their live-field behavior.

Particle simulation keeps its existing fixed-tick behavior. The current
`ParticleManager::Update` implementation ignores its `dt` argument; the validation
work does not turn particle parameters into a variable-timestep integration API.

Destroyed actors and removed component instances cannot use cached lifecycle or
emission methods to recreate their native resources. Existing stale-reference
no-op behavior need not become a new mandatory exception. Manually destroying a
still-registered live emitter is different: later initialization must revalidate
its configuration before creating a replacement.

## Regression coverage and limits

`agent_native_validation_test.py RUNTIME` generates an isolated project and uses
actual supervisor/worker processes, Lua `pcall`, Rigidbody methods and property
writes. Each rejected operation compares prior native/config values. Subsequent
real physics ticks check finite observations and valid velocity/force movement.
The finite-but-huge gravity regression continues for three ticks after rejection.
An unprotected bad setter verifies that an ordinary callback error remains an
error response while later valid requests and explicit reset remain usable.

Particle cases exercise integer/float rejection, the NaN sentinel, temporary
reversed endpoints, a rejected reserve overflow and tiny successful manual
bursts. They do not allocate a million particles to test the cap. Cached native
references remain usable after normal rejection. After actor destruction, real
raycasts check that stale body calls do not recreate fixtures, while stale
particle lifecycle calls must leave the worker healthy. Agent observations do
not expose internal emitter counts or emitted color buffers: native particle
tests provide the resource-liveness, capacity and color assertions for those
internal states.

The dangerous inputs must only run against a build containing their rejection
guards. Python syntax checks, native unit tests, real agent continuation, renderer
checks and the final build matrix are distinct evidence. This document describes
the boundary and workloads, without asserting that an unfinished matrix passed.
