#ifndef RIGIDBODY_H
#define RIGIDBODY_H

#include "scene/ActorHandle.h"
#include "physics/PhysicsIds.h"
#include "core/Vector2.h"
#include <string>
#include <vector>

struct Actor;

class Rigidbody {
public:
    Rigidbody() = default;
    Rigidbody(const Rigidbody &) = default;

    Actor *actor = nullptr;
    ActorHandle actor_handle;
    std::string key = "";
    bool enabled = true;

    // initial transform and body config
    // 初始变换和刚体配置
    float x = 0.0f;
    float y = 0.0f;
    std::string body_type = "dynamic";
    // Runtime-only physics hierarchy override. Authoring data continues to live
    // in `body_type`; this field only controls how the live Box2D body behaves.
    std::string effective_body_type = "";
    bool precise = true;
    float gravity_scale = 1.0f;
    float density = 1.0f;
    float angular_friction = 0.3f;
    // Linear velocity damping, like angular_friction for rotation.
    float linear_friction = 0.0f;
    float rotation = 0.0f;

    // collider and trigger settings
    // collider 和 trigger 配置
    bool has_collider = true;
    bool has_trigger = true;
    // box (width x height), circle (radius), capsule (vertical, radius and
    // total height) or polygon (convex hull of collider_points, local x/y pairs).
    std::string collider_type = "box";
    float width = 1.0f;
    float height = 1.0f;
    float radius = 0.5f;
    std::vector<float> collider_points;
    std::string trigger_type = "box";
    float trigger_width = 1.0f;
    float trigger_height = 1.0f;
    float trigger_radius = 0.5f;
    float friction = 0.3f;
    float bounciness = 0.3f;

    // Collision filtering for all of this body's shapes: the body is on layer
    // collision_layer (1..31) and touches shapes whose layer bit is in
    // collision_mask; both sides must accept each other.
    int collision_layer = 1;
    int collision_mask = kAllCollisionLayers;
    static constexpr int kAllCollisionLayers = 0x7FFFFFFF;

    // backing Box2D body, b2_nullBodyId until OnStart
    // 底层 Box2D body
    b2BodyId body = b2_nullBodyId;
    // Whether `body` is a live body of the current World's physics world.
    bool HasBody() const;

    // Full serialized configuration and derived mass/inertia/gravity checks.
    // Numeric authoring setters validate one field; this validates combinations
    // before fixtures/body state are created or their effective type changes.
    void ValidateConfiguration() const;
    static float ValidateNumericProperty(const char *name, double value);

    void OnStart();
    void OnDestroy();

    // runtime physics controls
    // 运行时物理控制接口
    void AddForce(const Vector2 &force);
    void SetVelocity(const Vector2 &velocity);
    void SetPosition(const Vector2 &position);
    void SetRotation(float degrees_clockwise);
    void SetEffectiveBodyType(const std::string &type_name);
    // Lifecycle-only: apply the authoring enabled flag at an unlocked boundary.
    void ApplyEnabledState();
    void SetAngularVelocity(float degrees_clockwise);
    void SetGravityScale(float scale);
    void SetUpDirection(Vector2 direction);
    void SetRightDirection(Vector2 direction);
    void ApplyImpulse(const Vector2 &impulse);
    // Clockwise-positive torque, matching rotation.
    void AddTorque(float torque);
    float GetMass() const;
    // Box2D filter for this body's shapes.
    b2Filter GetCollisionFilter() const;
    static void ValidateCollisionLayer(double layer);
    static void ValidateCollisionMask(double mask);
    // Polygon points must be 3..8 finite x/y pairs forming a convex hull.
    static void ValidateColliderPoints(const std::vector<float> &points);

    Vector2 GetPosition() const;
    float GetRotation() const;
    std::string GetEffectiveBodyType() const;
    Vector2 GetVelocity() const;
    float GetAngularVelocity() const;
    float GetGravityScale() const;
    Vector2 GetUpDirection() const;
    Vector2 GetRightDirection() const;

    // physics world of the current World. StepPhysicsWorld also delivers the
    // step's contact and trigger events.
    // 当前 World 的物理世界生命周期
    static void EnsurePhysicsWorld();
    static void DestroyPhysicsWorld();
    static bool HasPhysicsWorld();
    static void StepPhysicsWorld();

private:
    bool HasLiveRuntimeBinding() const;
    // The physics world `body` was created in; world ids carry a generation,
    // body ids do not.
    b2WorldId body_world_ = b2_nullWorldId;
    Vector2 accumulated_force_{0.0f, 0.0f};
    std::uint64_t force_step_ = 0;
};

#endif
