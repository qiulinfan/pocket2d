#ifndef PHYSICS_QUERIES_H
#define PHYSICS_QUERIES_H

#include "core/Vector2.h"
#include "physics/PhysicsIds.h"
#include "physics/RayCast.h"
#include "scene/ActorHandle.h"
#include <optional>
#include <string>
#include <vector>

class Rigidbody;

// World queries, kinematic character movement and joints for the current
// World's physics world. Results never include phantom shapes.
namespace PhysicsQueries {

// Actors with a collider (and, if requested, a trigger) overlapping the area,
// each once, in ascending UID order.
std::vector<ActorHandle> OverlapCircle(const Vector2 &center, float radius, bool include_triggers);
std::vector<ActorHandle> OverlapBox(const Vector2 &center, float width, float height, bool include_triggers);

// Nearest collider hit by a circle swept from origin along direction.
std::optional<RayCast::HitResult> CircleCast(const Vector2 &origin, float radius,
                                             const Vector2 &direction, float distance);

Vector2 GetGravity();
void SetGravity(const Vector2 &gravity);

// Moves a kinematic body by up to `delta`, sliding along colliders its
// collision mask accepts, and returns the movement applied. The body's own
// collider (circle, capsule or box bound) is swept as a capsule.
Vector2 MoveCharacter(Rigidbody &body, const Vector2 &delta);

} // namespace PhysicsQueries

// A Box2D joint between two started Rigidbodies. Box2D destroys joints with
// either body; IsValid reports whether this one still exists.
class PhysicsJoint {
public:
    PhysicsJoint() = default;
    static PhysicsJoint Connect(const std::string &kind, Rigidbody &a, Rigidbody &b,
                                const Vector2 &anchor, const Vector2 &anchor_or_axis);

    bool IsValid() const;
    void Destroy();
    std::string GetKind() const { return kind_; }
    // Motor speed is per second: degrees for revolute, meters otherwise.
    void SetMotor(bool enabled, float speed, float max_force);
    // Limits: degrees for revolute, meters (length or translation) otherwise.
    void SetLimits(bool enabled, float lower, float upper);
    void SetSpring(bool enabled, float hertz, float damping_ratio);

private:
    b2JointId joint_ = b2_nullJointId;
    b2WorldId world_ = b2_nullWorldId;
    std::string kind_;
};

#endif
