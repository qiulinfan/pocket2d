# Actor API

The `Actor` type is both:

- an instance class for live actors
- a static namespace for actor lookup / creation helpers

## Instance methods

### Identity and hierarchy

| Method | Description |
| --- | --- |
| `actor:GetName()` | Returns the actor name |
| `actor:GetUID()` | Returns the stable runtime/editor actor UID |
| `actor:IsValid()` / `actor.is_valid` | Whether this handle still identifies a live actor |
| `actor:SetParent(parent_actor)` | Reparents the actor in the live runtime world |
| `actor:GetChildCount()` | Returns the number of direct children |
| `actor:GetChildren()` | Returns direct children as a Lua array |

### Component lookup

| Method | Description |
| --- | --- |
| `actor:GetComponentByKey(key)` | Returns the component with that key, or `nil` |
| `actor:GetComponent(type_name)` | Returns the first component of that type, or `nil` |
| `actor:GetComponents(type_name)` | Returns all components of that type as a Lua array |
| `actor:GetComponentInChildren(type_name)` | Returns the first matching component on self or descendants |
| `actor:GetComponentsInChildren(type_name)` | Returns all matching components on self and descendants |

### Component mutation

| Method | Description |
| --- | --- |
| `actor:AddComponent(type_name)` | Adds a component instance and returns it |
| `actor:RemoveComponent(component_ref)` | Removes a component instance |

## Static `Actor` namespace

| Function | Description |
| --- | --- |
| `Actor.Instantiate(template_name)` | Spawns an actor from a template |
| `Actor.Destroy(actor)` | Requests destroying the actor at frame end |
| `Actor.Find(name)` | Returns the first actor with the given name |
| `Actor.FindAll(name)` | Returns all matching actors as a Lua array |

## Notes

- Lua actors are generation-checked handles. Caching an actor across another
  actor's destruction is safe. Destroying that actor invalidates its handle;
  reusing the same scene UID later does not revive an old handle.
- Handles for the same UID and runtime generation compare equal with `==`.
  `component.actor`, collision `other`, raycast `actor`, hierarchy lookups and
  `Actor.Find` all use the same identity rules.
- Check `IsValid()` before using a cached actor after a lifecycle change.
  This does not make arbitrary cached component objects or Lua table contents
  into a durable savegame format.

- `GetChildren()` only returns direct children.
- `GetComponentInChildren()` and `GetComponentsInChildren()` include `self`
  first, then descendants.
- Runtime child queries are derived from parent links on demand; there is no
  separate permanent children tree in the runtime core.

## Runtime creation failure boundary

`Actor.Instantiate` prepares the complete candidate before returning a findable
handle. A rejected candidate never runs gameplay `OnDestroy`. Successfully
published actors retain their established lifecycle behavior, including destruction
before their first start. A newly rejected template can be repaired on disk and
retried; successfully cached templates retain the existing cache lifetime. Failed
attempts may reserve unused UIDs or generated component keys. See
[runtime actor preparation](../development/runtime-actor-preparation.md) for
activation timing and the separate fatal publication boundary.
