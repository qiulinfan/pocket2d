#ifndef PHYSICS_QUERIES_H
#define PHYSICS_QUERIES_H

#include "core/Vector2.h"
#include "physics/PhysicsIds.h"
#include "physics/RayCast.h"
#include "scene/ActorHandle.h"
#include <functional>
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

// Moves a kinematic body by up to `delta` against colliders its collision
// mask accepts and returns the movement applied. Circle and capsule movers
// slide along any surface; a box mover moves one axis at a time (x, then y)
// with square corners, as tile platformers expect; its rotation is ignored.
Vector2 MoveCharacter(Rigidbody &body, const Vector2 &delta);

// What occupies a cell centre for path finding: nothing, only the walker's
// clearance margin around an obstacle, or the obstacle itself.
enum class PathCell { Free, Clearance, Solid };

// Grid A* from `from` toward `to` over cells of `cell` world units (8-way, no
// corner cutting), at most `max_nodes` expansions. `occupancy` classifies a
// cell centre. Solid cells never pass. Clearance cells pass only within
// ceil(clearance / cell) + 1 cells of the start or the goal, so a walker
// standing closer to a wall than its clearance can step out and a goal beside
// a wall can be reached; a step into one costs four free steps, so the path
// leaves the margin at once rather than running along the wall. The start
// cell is always free. Returns the waypoint centres after the start;
// when the goal is not reached, the path to the expanded cell nearest the goal
// (empty when none is nearer).
std::vector<Vector2> FindPath(const Vector2 &from, const Vector2 &to, float cell, int max_nodes, float clearance,
                              const std::function<PathCell(float, float)> &occupancy);

// Connected walkable regions of `points` under FindPath's cell rules: one
// 8-way flood (no corner cutting) per region, each expanding at most
// `max_nodes` cells. Clearance cells pass within ceil(clearance / cell) + 1
// cells of any point (each may be a path's start or goal); a point's own cell
// is always free. Returns a 1-based label per point, numbered in order of first
// appearance; equal labels share a region. A flood stopped by `max_nodes`
// leaves the points it did not reach to floods of their own, which then stop at
// its cells, so a node limit can split a region but never joins two.
std::vector<int> PathRegions(const std::vector<Vector2> &points, float cell, int max_nodes, float clearance,
                             const std::function<PathCell(float, float)> &occupancy);

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
