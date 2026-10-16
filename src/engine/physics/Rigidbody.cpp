#include "scripting/LuaBoundary.h"
#include "core/Profile.h"
#include "physics/Rigidbody.h"
#include "core/World.h"
#include "physics/PhysicsMutationGuard.h"
#include "scene/Actor.h"
#include "scripting/ComponentManager.h"
#include "physics/RayCast.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr float kPhysicsSyncEpsilon = 0.0001f;
constexpr double kFloatLimit = static_cast<double>(std::numeric_limits<float>::max()) / 16.0;
constexpr double kSquaredLimit = static_cast<double>(std::numeric_limits<float>::max()) / 256.0;
constexpr float kStep = 1.0f / 60.0f;
constexpr int kSubSteps = 4;
const Vector2 kSentinel(-999.0f, -999.0f);
// Physics step counter used to scope same-step force accumulation; per World.
struct ForceStep { std::uint64_t value = 0; };
std::uint64_t &CurrentForceStep() { return World::Current().State<ForceStep>().value; }

b2Vec2 ToBox2D(const Vector2 &value) { return {value.x, value.y}; }
Vector2 FromBox2D(const b2Vec2 &value) { return {value.x, value.y}; }

void Require(bool valid,const char *message) {
    if(!valid) throw std::invalid_argument(std::string("Rigidbody: ")+message);
}
void FiniteVector(const Vector2 &value) {
    Require(std::isfinite(value.x)&&std::isfinite(value.y),"vector components must be finite");
}
void SquaredVector(double x,double y,const char *message) {
    Require(std::isfinite(x)&&std::isfinite(y)&&x*x+y*y<=kSquaredLimit,message);
}
void Position(const Vector2 &position) {
    FiniteVector(position);
    Require(std::abs(position.x)<=1e8f&&std::abs(position.y)<=1e8f,"position must remain within +/-1e8");
}
void PositiveMass(double value,const char *message) {
    // Box2D stores both a quantity and its reciprocal in float. Positive
    // subnormal mass/inertia is not safe merely because its upper bound fits.
    Require(std::isfinite(value)&&value>=1.0/kFloatLimit&&value<=kFloatLimit,message);
}
struct Mass { double mass=0,inertia=0; };
// Box2D 3 reports rotational inertia about the centre of mass; the engine's
// shapes are centred on the body origin, so it equals inertia about the origin.
// Capsules are vertical: total height includes both end caps.
b2Capsule MakeCapsule(float height,float radius) {
    const float half=std::max(height*0.5f-radius,0.0f);
    return {{0.0f,-half},{0.0f,half},radius};
}
b2Polygon MakeHullPolygon(const std::vector<float> &points) {
    std::vector<b2Vec2> vertices;
    for(std::size_t i=0;i+1<points.size();i+=2) vertices.push_back({points[i],points[i+1]});
    const b2Hull hull=b2ComputeHull(vertices.data(),static_cast<int>(vertices.size()));
    Require(hull.count>=3,"collider_points must form a convex hull with at least three corners");
    return b2MakePolygon(&hull,0.0f);
}
Mass ActualShapeMass(const b2MassData &actual,float density) {
    Require(std::isfinite(actual.mass)&&std::isfinite(actual.rotationalInertia)&&b2IsValidVec2(actual.center),"derived fixture mass/center/inertia is not finite");
    if(density>0) { PositiveMass(actual.mass,"float fixture mass has an unsafe reciprocal");PositiveMass(actual.rotationalInertia,"float fixture inertia has an unsafe reciprocal"); }
    return {actual.mass,actual.rotationalInertia+static_cast<double>(actual.mass)*(actual.center.x*actual.center.x+actual.center.y*actual.center.y)};
}
Mass ShapeMass(float width,float height,float radius,bool circle,float density) {
    const double area=circle?static_cast<double>(B2_PI)*radius*radius:static_cast<double>(width)*height;
    const double expected_mass=static_cast<double>(density)*area;
    const double expected_inertia=expected_mass*(circle?0.5*radius*radius:(static_cast<double>(width)*width+static_cast<double>(height)*height)/12.0);
    if(density>0) {
        PositiveMass(expected_mass,"fixture mass or its reciprocal exceeds supported float range");
        PositiveMass(expected_inertia,"fixture inertia or its reciprocal exceeds supported float range");
    }
    b2MassData actual;
    if(circle) { const b2Circle shape{{0,0},radius};actual=b2ComputeCircleMass(&shape,density); }
    else { const b2Polygon shape=b2MakeBox(width*0.5f,height*0.5f);actual=b2ComputePolygonMass(&shape,density); }
    Require(std::isfinite(actual.mass)&&std::isfinite(actual.rotationalInertia)&&b2IsValidVec2(actual.center),"derived fixture mass/center/inertia is not finite");
    if(density>0) { PositiveMass(actual.mass,"float fixture mass has an unsafe reciprocal");PositiveMass(actual.rotationalInertia,"float fixture inertia has an unsafe reciprocal"); }
    return {actual.mass,actual.rotationalInertia};
}
Vector2 Gravity() { return RayCast::HasPhysicsWorld()?FromBox2D(b2World_GetGravity(RayCast::GetPhysicsWorld())):Vector2(0,9.8f); }
Vector2 Integration(double mass,double scale,const Vector2 &gravity,double force_x,double force_y,const Vector2 &velocity) {
    FiniteVector(gravity);FiniteVector(velocity);
    Require(std::isfinite(scale),"gravity scale must be finite");
    SquaredVector(scale*gravity.x,scale*gravity.y,"derived gravity acceleration exceeds finite squared-magnitude range");
    if(mass>0) PositiveMass(mass,"live mass has an unsafe reciprocal");
    Require(std::isfinite(mass)&&mass>=0,"live mass is invalid");
    const double scaled_mass=scale*mass;
    // Box2D integrates h*invMass*(mass*gravityScale*gravity+force) on each
    // substep; over one step the products are the ones checked here.
    Require(std::isfinite(scaled_mass)&&std::abs(scaled_mass)<=kFloatLimit,"gravityScale * mass exceeds supported float range");
    const double gravity_x=scaled_mass*gravity.x,gravity_y=scaled_mass*gravity.y;
    Require(std::abs(gravity_x)<=kFloatLimit&&std::abs(gravity_y)<=kFloatLimit,"gravity force exceeds supported float range");
    const double total_x=gravity_x+force_x,total_y=gravity_y+force_y;
    Require(std::isfinite(total_x)&&std::isfinite(total_y)&&std::abs(force_x)<=kFloatLimit&&std::abs(force_y)<=kFloatLimit&&
            std::abs(total_x)<=kFloatLimit&&std::abs(total_y)<=kFloatLimit,"accumulated force exceeds supported float range");
    const double gain=mass>0?static_cast<double>(kStep)*(1.0/mass):0;
    const double next_x=velocity.x+gain*total_x,next_y=velocity.y+gain*total_y;
    SquaredVector(next_x,next_y,"force/gravity integration would exceed finite velocity norms");
    return {static_cast<float>(next_x),static_cast<float>(next_y)};
}
std::vector<b2ShapeId> Shapes(b2BodyId body) {
    std::vector<b2ShapeId> shapes(static_cast<std::size_t>(b2Body_GetShapeCount(body)));
    shapes.resize(static_cast<std::size_t>(b2Body_GetShapes(body,shapes.data(),static_cast<int>(shapes.size()))));
    return shapes;
}
struct Geometry { double extent=0,friction=0,restitution=0; };
Geometry Describe(b2BodyId body) {
    Geometry result;
    for(const b2ShapeId shape:Shapes(body)) {
        if(b2Shape_GetType(shape)==b2_circleShape) {
            const b2Circle circle=b2Shape_GetCircle(shape);
            result.extent=std::max(result.extent,std::hypot(static_cast<double>(circle.center.x),circle.center.y)+circle.radius);
        } else if(b2Shape_GetType(shape)==b2_polygonShape) {
            const b2Polygon polygon=b2Shape_GetPolygon(shape);
            for(int i=0;i<polygon.count;++i) result.extent=std::max(result.extent,std::hypot(static_cast<double>(polygon.vertices[i].x),polygon.vertices[i].y)+polygon.radius);
        }
        result.friction=std::max(result.friction,static_cast<double>(b2Shape_GetFriction(shape)));
        result.restitution=std::max(result.restitution,static_cast<double>(b2Shape_GetRestitution(shape)));
    }
    return result;
}
void Motion(double mass,double inertia,const Geometry &geometry,const Vector2 &velocity,double angular_velocity) {
    SquaredVector(velocity.x,velocity.y,"linear velocity exceeds finite squared-magnitude range");
    Require(std::isfinite(angular_velocity)&&angular_velocity*angular_velocity<=kSquaredLimit,"angular velocity exceeds finite squared-magnitude range");
    const double speed=std::hypot(static_cast<double>(velocity.x),velocity.y)+std::abs(angular_velocity)*geometry.extent;
    Require(std::isfinite(speed)&&speed*speed<=kSquaredLimit,"fixture point velocity exceeds finite squared-magnitude range");
    const double material=(1+geometry.friction)*(1+geometry.restitution);
    // Contact impulses involve mass*relative speed, material multipliers and
    // lever arms; reject dangerous own-body products before the solver sees
    // them. This is not a proof for all heterogeneous contact/joint systems.
    const double momentum=mass*speed*material*(1+geometry.extent);
    const double angular_momentum=inertia*std::abs(angular_velocity)*material;
    Require(std::isfinite(momentum)&&std::isfinite(angular_momentum)&&momentum<=kFloatLimit&&angular_momentum<=kFloatLimit,
            "mass/inertia and velocity would overflow contact impulse products");
}
void PointSpeed(b2BodyId body,const Vector2 &velocity,double angular_velocity) {
    Motion(b2Body_GetMass(body),b2Body_GetRotationalInertia(body),Describe(body),velocity,angular_velocity);
}
Mass ActualMass(b2BodyId body) {
    Mass total;
    double weighted_x=0,weighted_y=0;
    for(const b2ShapeId shape:Shapes(body)) {
        const float density=b2Shape_GetDensity(shape);
        Require(std::isfinite(density)&&density>=0,"live fixture density is invalid");
        if(density==0) continue;
        const b2MassData data=b2Shape_GetMassData(shape);
        // Inertia about the body origin, as the combination below expects.
        const double inertia=data.rotationalInertia+static_cast<double>(data.mass)*(data.center.x*data.center.x+data.center.y*data.center.y);
        PositiveMass(data.mass,"live fixture mass has an unsafe reciprocal");
        PositiveMass(inertia,"live fixture inertia has an unsafe reciprocal");
        Require(b2IsValidVec2(data.center),"live fixture center is not finite");
        weighted_x+=static_cast<double>(data.mass)*data.center.x;
        weighted_y+=static_cast<double>(data.mass)*data.center.y;
        Require(std::isfinite(weighted_x)&&std::isfinite(weighted_y)&&std::abs(weighted_x)<=kFloatLimit&&std::abs(weighted_y)<=kFloatLimit,
                "live mass-weighted center exceeds supported float range");
        total.mass+=data.mass;total.inertia+=inertia;
        PositiveMass(total.mass,"combined live mass has unsafe range/reciprocal");
        PositiveMass(total.inertia,"combined live inertia has unsafe range/reciprocal");
    }
    if(total.mass>0&&total.inertia>0) {
        const double center_x=weighted_x/total.mass,center_y=weighted_y/total.mass;
        total.inertia-=total.mass*(center_x*center_x+center_y*center_y);
        PositiveMass(total.inertia,"centered live inertia has unsafe range/reciprocal");
    }
    return total;
}

bool NearlyEqual(float a, float b) {
    return std::fabs(a - b) <= kPhysicsSyncEpsilon;
}

// Rotations are built and read in double precision and rounded to float.
// Box2D's b2MakeRot approximation is off by up to ~0.1 degree, which would be
// visible in authored rotations. Like the Box2D 2.4 body angle this relies on
// the platform's libm; see ADR 008.
b2Rot RotationFromRadians(float radians) {
    const double angle = radians;
    return {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
}
float RadiansOf(b2Rot rotation) {
    return static_cast<float>(std::atan2(static_cast<double>(rotation.s), static_cast<double>(rotation.c)));
}

float ClockwiseDegreesToBox2DRadians(float rotation_degrees) {
    return rotation_degrees * (B2_PI / 180.0f);
}

float Box2DRadiansToClockwiseDegrees(float rotation_radians) {
    return rotation_radians * (180.0f / B2_PI);
}

b2BodyType ParseBodyType(const std::string &body_type) {
    if (body_type == "static") return b2_staticBody;
    if (body_type == "kinematic") return b2_kinematicBody;
    if(body_type=="dynamic") return b2_dynamicBody;
    throw std::invalid_argument("Rigidbody body type must be dynamic/static/kinematic");
}

// Colliders produce contact events with other colliders. Triggers are sensors
// that detect only other triggers, as sensor visitors. Phantoms only give a
// shapeless body mass and are invisible to events and ray casts.
void CreateShape(Rigidbody &rigidbody, RayCast::FixtureKind kind, const std::string &type,
                 float width, float height, float radius) {
    b2ShapeDef shape_def = b2DefaultShapeDef();
    shape_def.density = rigidbody.density;
    shape_def.filter = rigidbody.GetCollisionFilter();
    shape_def.isSensor = kind != RayCast::FixtureKind::Collider;
    shape_def.enableSensorEvents = kind == RayCast::FixtureKind::Trigger;
    shape_def.enableContactEvents = kind == RayCast::FixtureKind::Collider;
    if (kind == RayCast::FixtureKind::Collider) {
        shape_def.material.friction = rigidbody.friction;
        shape_def.material.restitution = rigidbody.bounciness;
    }
    b2ShapeId shape;
    if (type == "circle") {
        const b2Circle geometry{{0.0f, 0.0f}, radius};
        shape = b2CreateCircleShape(rigidbody.body, &shape_def, &geometry);
    } else if (type == "capsule") {
        const b2Capsule geometry = MakeCapsule(height, radius);
        shape = b2CreateCapsuleShape(rigidbody.body, &shape_def, &geometry);
    } else if (type == "polygon") {
        const b2Polygon geometry = MakeHullPolygon(rigidbody.collider_points);
        shape = b2CreatePolygonShape(rigidbody.body, &shape_def, &geometry);
    } else {
        const b2Polygon geometry = b2MakeBox(width * 0.5f, height * 0.5f);
        shape = b2CreatePolygonShape(rigidbody.body, &shape_def, &geometry);
    }
    RayCast::RegisterShape(shape, rigidbody.actor, kind);
}

// ---------------------------------------------------------------------------
// Contact and trigger events
//
// Box2D reports touches after each step. Callbacks run from a copy of those
// events, so Lua may change the world while they are delivered. Contacts ended
// by a user operation between steps (disable, retype, destroy) are reported
// after the next step.
// ---------------------------------------------------------------------------

struct PendingEvent {
    bool trigger = false;
    bool enter = false;
    ActorHandle receiver;
    ActorHandle other;
    Vector2 point;
    Vector2 relative_velocity;
    Vector2 normal;
};

// Two overlapping triggers, one per sensing direction. A pair is reported
// while both bodies are enabled and at least one is dynamic, the rule Box2D
// 2.4 contacts followed; sensors in Box2D 3 ignore body type.
struct TriggerPair {
    b2ShapeId sensor;
    b2ShapeId visitor;
    bool reported = false;
};
struct TriggerState { std::map<std::pair<std::uint64_t, std::uint64_t>, TriggerPair> pairs; };
TriggerState &Triggers() { return World::Current().State<TriggerState>(); }

ActorHandle HandleOf(const RayCast::FixtureMetadata &metadata) {
    return metadata.actor != nullptr ? ActorHandle(metadata.actor) : metadata.actor_handle;
}

Vector2 VelocityOf(b2ShapeId shape) {
    if (!b2Shape_IsValid(shape)) return {};
    return FromBox2D(b2Body_GetLinearVelocity(b2Shape_GetBody(shape)));
}

bool ShouldReportTrigger(const TriggerPair &pair) {
    if (!b2Shape_IsValid(pair.sensor) || !b2Shape_IsValid(pair.visitor)) return false;
    const b2BodyId a = b2Shape_GetBody(pair.sensor), b = b2Shape_GetBody(pair.visitor);
    if (!b2Body_IsEnabled(a) || !b2Body_IsEnabled(b)) return false;
    return b2Body_GetType(a) == b2_dynamicBody || b2Body_GetType(b) == b2_dynamicBody;
}

void CollectContactEvents(b2WorldId world, std::vector<PendingEvent> &events) {
    const b2ContactEvents contacts = b2World_GetContactEvents(world);
    const auto collide = [&](b2ShapeId shape_a, b2ShapeId shape_b, bool enter,
                             const b2Manifold *manifold) {
        const auto *a = RayCast::FindShapeMetadata(shape_a);
        const auto *b = RayCast::FindShapeMetadata(shape_b);
        if (a == nullptr || b == nullptr) return;
        if (a->kind != RayCast::FixtureKind::Collider || b->kind != RayCast::FixtureKind::Collider) return;
        Vector2 point = kSentinel, normal = kSentinel;
        if (manifold != nullptr && manifold->pointCount > 0) {
            point = FromBox2D(manifold->points[0].point);
            normal = FromBox2D(manifold->normal);
        }
        const Vector2 relative = VelocityOf(shape_a) - VelocityOf(shape_b);
        events.push_back({false, enter, HandleOf(*a), HandleOf(*b), point, relative, normal});
        events.push_back({false, enter, HandleOf(*b), HandleOf(*a), point, relative, normal});
    };
    for (int i = 0; i < contacts.beginCount; ++i) {
        const b2ContactBeginTouchEvent &event = contacts.beginEvents[i];
        collide(event.shapeIdA, event.shapeIdB, true, &event.manifold);
    }
    for (int i = 0; i < contacts.endCount; ++i) {
        const b2ContactEndTouchEvent &event = contacts.endEvents[i];
        collide(event.shapeIdA, event.shapeIdB, false, nullptr);
    }
}

void CollectTriggerEvents(b2WorldId world, std::vector<PendingEvent> &events) {
    auto &pairs = Triggers().pairs;
    const auto report = [&](const TriggerPair &pair, bool enter) {
        const auto *sensor = RayCast::FindShapeMetadata(pair.sensor);
        const auto *visitor = RayCast::FindShapeMetadata(pair.visitor);
        if (sensor == nullptr || visitor == nullptr) return;
        events.push_back({true, enter, HandleOf(*sensor), HandleOf(*visitor), kSentinel,
                          VelocityOf(pair.sensor) - VelocityOf(pair.visitor), kSentinel});
    };
    const b2SensorEvents sensors = b2World_GetSensorEvents(world);
    for (int i = 0; i < sensors.endCount; ++i) {
        const b2SensorEndTouchEvent &event = sensors.endEvents[i];
        const auto found = pairs.find({b2StoreShapeId(event.sensorShapeId), b2StoreShapeId(event.visitorShapeId)});
        if (found == pairs.end()) continue;
        if (found->second.reported) report(found->second, false);
        pairs.erase(found);
    }
    for (int i = 0; i < sensors.beginCount; ++i) {
        const b2SensorBeginTouchEvent &event = sensors.beginEvents[i];
        const auto *sensor = RayCast::FindShapeMetadata(event.sensorShapeId);
        const auto *visitor = RayCast::FindShapeMetadata(event.visitorShapeId);
        if (sensor == nullptr || visitor == nullptr ||
            sensor->kind != RayCast::FixtureKind::Trigger || visitor->kind != RayCast::FixtureKind::Trigger) continue;
        pairs.try_emplace({b2StoreShapeId(event.sensorShapeId), b2StoreShapeId(event.visitorShapeId)},
                          TriggerPair{event.sensorShapeId, event.visitorShapeId, false});
    }
    // Body type and enabled changes do not end sensor overlaps in Box2D 3.
    for (auto &[key, pair] : pairs) {
        const bool should = ShouldReportTrigger(pair);
        if (should == pair.reported) continue;
        report(pair, should);
        pair.reported = should;
    }
}

void DispatchPhysicsEvents(b2WorldId world) {
    std::vector<PendingEvent> events;
    CollectContactEvents(world, events);
    CollectTriggerEvents(world, events);
    // Direct native destruction stays rejected while callbacks run, so later
    // events in this batch never refer to shapes a callback destroyed.
    PhysicsMutationGuard dispatch;
    for (const PendingEvent &event : events) {
        if (event.trigger)
            ComponentManager::QueueTriggerEvent(event.receiver, event.other, event.point,
                                                event.relative_velocity, event.normal, event.enter);
        else
            ComponentManager::QueueCollisionEvent(event.receiver, event.other, event.point,
                                                  event.relative_velocity, event.normal, event.enter);
    }
}

} // namespace

float Rigidbody::ValidateNumericProperty(const char *name,double value) {
    Require(std::isfinite(value)&&std::abs(value)<=std::numeric_limits<float>::max(),"numeric value must be finite and representable as float");
    const std::string_view field(name);
    if(field=="x"||field=="y") Require(std::abs(value)<=1e8,"position must remain within +/-1e8");
    else if(field=="width"||field=="height"||field=="radius"||field=="trigger_width"||field=="trigger_height"||field=="trigger_radius")
        Require(value>=0.001&&value<=1e6,"dimensions must be 0.001..1000000");
    else if(field=="density"||field=="angular_friction"||field=="linear_friction") Require(value>=0&&value<=kFloatLimit,"density/damping must be nonnegative with finite arithmetic headroom");
    else if(field=="friction"||field=="bounciness")
        Require(value>=0&&value*value<=kSquaredLimit,"friction/restitution must be nonnegative with finite product headroom");
    else if(field=="gravity_scale") SquaredVector(0,value*static_cast<double>(9.8f),"gravity scale exceeds finite acceleration norms");
    else if(field=="rotation") Require(std::abs(value)<=kFloatLimit,"rotation must retain finite conversion headroom");
    else throw std::invalid_argument("unknown Rigidbody numeric property");
    const float narrowed=static_cast<float>(value);
    Require(value==0||narrowed!=0,"numeric value underflows float range");
    return narrowed;
}
void Rigidbody::ValidateCollisionLayer(double layer) {
    Require(std::isfinite(layer)&&std::floor(layer)==layer&&layer>=1&&layer<=31,"collision_layer must be an integer 1..31");
}
void Rigidbody::ValidateCollisionMask(double mask) {
    Require(std::isfinite(mask)&&std::floor(mask)==mask&&mask>=0&&mask<=kAllCollisionLayers,"collision_mask must be an integer 0..2147483647");
}
void Rigidbody::ValidateColliderPoints(const std::vector<float> &points) {
    Require(points.size()%2==0&&points.size()>=6&&points.size()<=2*B2_MAX_POLYGON_VERTICES,"collider_points needs 3..8 x/y pairs");
    for(const float value:points) Require(std::isfinite(value)&&std::abs(value)<=1e6f,"collider_points must be finite within +/-1e6");
    MakeHullPolygon(points);
}
b2Filter Rigidbody::GetCollisionFilter() const {
    b2Filter filter=b2DefaultShapeDef().filter;
    filter.categoryBits=std::uint64_t{1}<<(collision_layer-1);
    filter.maskBits=static_cast<std::uint64_t>(static_cast<std::uint32_t>(collision_mask));
    return filter;
}
void Rigidbody::ValidateConfiguration() const {
    ParseBodyType(body_type);
    if(!effective_body_type.empty()&&effective_body_type!="none") ParseBodyType(effective_body_type);
    Require((collider_type=="box"||collider_type=="circle"||collider_type=="capsule"||collider_type=="polygon")&&
            (trigger_type=="box"||trigger_type=="circle"),"collider_type must be box/circle/capsule/polygon and trigger_type box/circle");
    ValidateCollisionLayer(collision_layer);
    ValidateCollisionMask(collision_mask);
    if(has_collider&&collider_type=="polygon") ValidateColliderPoints(collider_points);
    if(has_collider&&collider_type=="capsule") Require(height>=2*radius,"capsule height must be at least twice its radius");
    for(const auto &[name,value]:std::initializer_list<std::pair<const char *,float>>{
        {"x",x},{"y",y},{"rotation",rotation},{"gravity_scale",gravity_scale},{"density",density},{"angular_friction",angular_friction},{"linear_friction",linear_friction},
        {"width",width},{"height",height},{"radius",radius},{"trigger_width",trigger_width},{"trigger_height",trigger_height},
        {"trigger_radius",trigger_radius},{"friction",friction},{"bounciness",bounciness}}) ValidateNumericProperty(name,value);
    Mass total;
    const auto add=[&](Mass value){total.mass+=value.mass;total.inertia+=value.inertia;
        if(total.mass>0) PositiveMass(total.mass,"combined body mass has unsafe range/reciprocal");
        if(total.inertia>0) PositiveMass(total.inertia,"combined body inertia has unsafe range/reciprocal");};
    if(has_collider&&collider_type=="capsule") { const b2Capsule capsule=MakeCapsule(height,radius);add(ActualShapeMass(b2ComputeCapsuleMass(&capsule,density),density)); }
    else if(has_collider&&collider_type=="polygon") { const b2Polygon polygon=MakeHullPolygon(collider_points);add(ActualShapeMass(b2ComputePolygonMass(&polygon,density),density)); }
    else if(has_collider) add(ShapeMass(width,height,radius,collider_type=="circle",density));
    if(has_trigger) add(ShapeMass(trigger_width,trigger_height,trigger_radius,trigger_type=="circle",density));
    if(!has_collider&&!has_trigger) add(ShapeMass(width,height,radius,false,density));
    const auto predicted=Integration(total.mass,gravity_scale,Gravity(),0,0,Vector2());
    Geometry geometry;
    geometry.friction=has_collider?friction:0;
    geometry.restitution=has_collider?bounciness:0;
    // Box2D 3 boxes have no skin radius.
    if(has_collider&&collider_type=="capsule") geometry.extent=std::max(height*0.5,static_cast<double>(radius));
    else if(has_collider&&collider_type=="polygon") {
        for(std::size_t i=0;i+1<collider_points.size();i+=2) geometry.extent=std::max(geometry.extent,std::hypot(static_cast<double>(collider_points[i]),collider_points[i+1]));
    } else if(has_collider) geometry.extent=collider_type=="circle"?radius:std::hypot(static_cast<double>(width),height)*0.5;
    if(has_trigger) geometry.extent=std::max(geometry.extent,trigger_type=="circle"?static_cast<double>(trigger_radius):std::hypot(static_cast<double>(trigger_width),trigger_height)*0.5);
    if(!has_collider&&!has_trigger) geometry.extent=std::hypot(static_cast<double>(width),height)*0.5;
    Motion(total.mass,total.inertia,geometry,predicted,0);
}

// Box2D keeps all worlds in one process-wide array; creating and destroying
// worlds is not thread-safe. Operations within one world need no lock.
static std::mutex &WorldRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

// physics world lifecycle of the current World
void Rigidbody::EnsurePhysicsWorld() {
    if (RayCast::HasPhysicsWorld()) return;
    b2WorldDef world_def = b2DefaultWorldDef();
    world_def.gravity = {0.0f, 9.8f};
    std::lock_guard lock(WorldRegistryMutex());
    RayCast::SetPhysicsWorld(b2CreateWorld(&world_def));
}

void Rigidbody::DestroyPhysicsWorld() {
    if (RayCast::HasPhysicsWorld()) {
        std::lock_guard lock(WorldRegistryMutex());
        b2DestroyWorld(RayCast::GetPhysicsWorld());
    }
    RayCast::ClearPhysicsState();
    Triggers().pairs.clear();
    ++CurrentForceStep();
}

bool Rigidbody::HasPhysicsWorld() {
    return RayCast::HasPhysicsWorld();
}

// Step once, then deliver the step's contact and trigger events.
void Rigidbody::StepPhysicsWorld() {
    POCKET_ZONE("Box2D step and events");
    if (!RayCast::HasPhysicsWorld()) return;
    const b2WorldId world = RayCast::GetPhysicsWorld();
    b2World_Step(world, kStep, kSubSteps);
    ++CurrentForceStep();
    RayCast::AdvanceStepIndex();
    DispatchPhysicsEvents(world);
    RayCast::ReleaseRetiredShapes(RayCast::GetStepIndex());
}

bool Rigidbody::HasBody() const {
    return B2_IS_NON_NULL(body) && RayCast::HasPhysicsWorld() &&
           body_world_ == RayCast::GetPhysicsWorld() && b2Body_IsValid(body);
}

bool Rigidbody::HasLiveRuntimeBinding() const {
    if (actor_handle.generation == 0) return true;
    if (!actor_handle.IsValid()) return false;
    return ComponentManager::FindBuiltinComponent(actor_handle.uid, key) == this;
}

// Create the Box2D body and shapes from the serialized runtime config.
void Rigidbody::OnStart() {
    // A Lua reference may outlive its owning actor. Lifecycle methods remain
    // callable from scripts, but must never recreate shapes with stale owners.
    if (!HasLiveRuntimeBinding()) return;
    if (HasBody()) return;
    ValidateConfiguration();
    EnsurePhysicsWorld();

    b2BodyDef body_def = b2DefaultBodyDef();
    body_def.type = ParseBodyType(GetEffectiveBodyType());
    body_def.position = {x, y};
    body_def.rotation = RotationFromRadians(ClockwiseDegreesToBox2DRadians(rotation));
    body_def.isBullet = precise;
    body_def.gravityScale = gravity_scale;
    body_def.angularDamping = angular_friction;
    body_def.linearDamping = linear_friction;
    body_def.isEnabled = enabled;

    body_world_ = RayCast::GetPhysicsWorld();
    body = b2CreateBody(body_world_, &body_def);

    if (has_collider)
        CreateShape(*this, RayCast::FixtureKind::Collider, collider_type, width, height, radius);
    if (has_trigger)
        CreateShape(*this, RayCast::FixtureKind::Trigger, trigger_type,
                    trigger_width, trigger_height, trigger_radius);
    if (!has_collider && !has_trigger)
        CreateShape(*this, RayCast::FixtureKind::Phantom, "box", width, height, radius);
    accumulated_force_.SetZero();force_step_=CurrentForceStep();
}

// Destroy the backing Box2D body and retire its shape records.
void Rigidbody::OnDestroy() {
    if (!HasBody()) { body = b2_nullBodyId; return; }
    if (PhysicsMutationGuard::IsActive())
        throw std::runtime_error("Rigidbody.OnDestroy cannot run during a physics contact callback; use actor:RemoveComponent(body) or Actor.Destroy(actor) for deferred cleanup");

    for (const b2ShapeId shape : Shapes(body)) RayCast::UnregisterShape(shape);
    b2DestroyBody(body);
    body = b2_nullBodyId;
    accumulated_force_.SetZero();force_step_=CurrentForceStep();
}

// runtime physics controls
Vector2 Rigidbody::GetPosition() const {
    if (HasBody()) return FromBox2D(b2Body_GetPosition(body));
    return Vector2(x, y);
}

float Rigidbody::GetRotation() const {
    if (HasBody()) return Box2DRadiansToClockwiseDegrees(RadiansOf(b2Body_GetRotation(body)));
    return rotation;
}

std::string Rigidbody::GetEffectiveBodyType() const {
    if (!effective_body_type.empty() && effective_body_type != "none") {
        return effective_body_type;
    }
    return body_type;
}

void Rigidbody::AddForce(const Vector2 &force) {
    FiniteVector(force);
    if (!HasBody()) return;
    if(b2Body_GetType(body)!=b2_dynamicBody) return;
    const auto previous=force_step_==CurrentForceStep()?accumulated_force_:Vector2();
    const double next_x=static_cast<double>(previous.x)+force.x,next_y=static_cast<double>(previous.y)+force.y;
    const auto predicted=Integration(b2Body_GetMass(body),b2Body_GetGravityScale(body),Gravity(),next_x,next_y,FromBox2D(b2Body_GetLinearVelocity(body)));
    PointSpeed(body,predicted,b2Body_GetAngularVelocity(body));
    b2Body_ApplyForceToCenter(body, ToBox2D(force), true);
    accumulated_force_.Set(static_cast<float>(next_x),static_cast<float>(next_y));force_step_=CurrentForceStep();
}

void Rigidbody::SetVelocity(const Vector2 &velocity) {
    FiniteVector(velocity);SquaredVector(velocity.x,velocity.y,"linear velocity exceeds finite squared-magnitude range");
    if (!HasBody()) return;
    const auto force=force_step_==CurrentForceStep()?accumulated_force_:Vector2();
    PointSpeed(body,velocity,b2Body_GetAngularVelocity(body));
    const auto predicted=Integration(b2Body_GetMass(body),b2Body_GetGravityScale(body),Gravity(),force.x,force.y,velocity);
    PointSpeed(body,predicted,b2Body_GetAngularVelocity(body));
    b2Body_SetLinearVelocity(body, ToBox2D(velocity));
}

void Rigidbody::SetPosition(const Vector2 &position) {
    Position(position);
    const auto current=GetPosition();
    const bool same_position = NearlyEqual(current.x, position.x) && NearlyEqual(current.y, position.y);
    x = position.x;
    y = position.y;

    if (!HasBody()) return;
    if (same_position) return;
    b2Body_SetTransform(body, ToBox2D(position), b2Body_GetRotation(body));
}

void Rigidbody::SetRotation(float degrees_clockwise) {
    ValidateNumericProperty("rotation",degrees_clockwise);
    const bool same_rotation = NearlyEqual(GetRotation(), degrees_clockwise);
    rotation = degrees_clockwise;

    if (!HasBody()) return;
    if (same_rotation) return;
    b2Body_SetTransform(body, b2Body_GetPosition(body),
                        RotationFromRadians(ClockwiseDegreesToBox2DRadians(degrees_clockwise)));
}

void Rigidbody::SetEffectiveBodyType(const std::string &type_name) {
    if(type_name.empty()||type_name=="none") { effective_body_type=type_name;return; }
    const b2BodyType requested_type=ParseBodyType(type_name);
    // The hierarchy bridge calls this every tick. Pending authoring fields do
    // not reconfigure existing shapes, and a no-op must not validate them.
    if(!HasBody()||b2Body_GetType(body)==requested_type) { effective_body_type=type_name;return; }
    const auto mass=ActualMass(body); // Retype consumes actual shapes, not pending raw config.
    Position(FromBox2D(b2Body_GetPosition(body)));
    ValidateNumericProperty("rotation",Box2DRadiansToClockwiseDegrees(RadiansOf(b2Body_GetRotation(body))));
    const auto velocity=requested_type==b2_staticBody?Vector2():FromBox2D(b2Body_GetLinearVelocity(body));
    const double angular=requested_type==b2_staticBody?0:b2Body_GetAngularVelocity(body);
    const auto predicted=Integration(requested_type==b2_dynamicBody?mass.mass:0,b2Body_GetGravityScale(body),Gravity(),0,0,velocity);
    Motion(requested_type==b2_dynamicBody?mass.mass:0,requested_type==b2_dynamicBody?mass.inertia:0,Describe(body),predicted,angular);
    b2Body_SetType(body, requested_type);
    effective_body_type = type_name;
    accumulated_force_.SetZero();force_step_=CurrentForceStep();
}

void Rigidbody::ApplyEnabledState() {
    if (!HasBody() || b2Body_IsEnabled(body) == enabled) return;
    if (enabled) b2Body_Enable(body);
    else b2Body_Disable(body);
}

void Rigidbody::SetAngularVelocity(float degrees_clockwise) {
    ValidateNumericProperty("rotation",degrees_clockwise);
    const auto radians=ClockwiseDegreesToBox2DRadians(degrees_clockwise);
    Require(static_cast<double>(radians)*radians<=kSquaredLimit,"angular velocity exceeds finite squared-magnitude range");
    if (!HasBody()) return;
    PointSpeed(body,FromBox2D(b2Body_GetLinearVelocity(body)),radians);
    b2Body_SetAngularVelocity(body, radians);
}

void Rigidbody::SetGravityScale(float scale) {
    ValidateNumericProperty("gravity_scale",scale);
    if(HasBody()) {
        const auto force=force_step_==CurrentForceStep()?accumulated_force_:Vector2();
        const auto predicted=Integration(b2Body_GetMass(body),scale,Gravity(),force.x,force.y,FromBox2D(b2Body_GetLinearVelocity(body)));
        PointSpeed(body,predicted,b2Body_GetAngularVelocity(body));
    }
    gravity_scale = scale;
    if (!HasBody()) return;
    b2Body_SetGravityScale(body, scale);
}

void Rigidbody::SetUpDirection(Vector2 direction) {
    FiniteVector(direction);
    if(direction.x==0&&direction.y==0) return;
    const float new_angle_radians=static_cast<float>(std::atan2(static_cast<double>(direction.x),-static_cast<double>(direction.y)));
    rotation = Box2DRadiansToClockwiseDegrees(new_angle_radians);

    if (!HasBody()) return;
    b2Body_SetTransform(body, b2Body_GetPosition(body), RotationFromRadians(new_angle_radians));
}

void Rigidbody::SetRightDirection(Vector2 direction) {
    FiniteVector(direction);
    if(direction.x==0&&direction.y==0) return;
    const float new_angle_radians=static_cast<float>(std::atan2(static_cast<double>(direction.x),-static_cast<double>(direction.y))-B2_PI/2.0);
    rotation = Box2DRadiansToClockwiseDegrees(new_angle_radians);

    if (!HasBody()) return;
    b2Body_SetTransform(body, b2Body_GetPosition(body), RotationFromRadians(new_angle_radians));
}

Vector2 Rigidbody::GetVelocity() const {
    if (HasBody()) return FromBox2D(b2Body_GetLinearVelocity(body));
    return Vector2(0.0f, 0.0f);
}

float Rigidbody::GetAngularVelocity() const {
    if (HasBody()) {
        return Box2DRadiansToClockwiseDegrees(b2Body_GetAngularVelocity(body));
    }
    return 0.0f;
}

float Rigidbody::GetGravityScale() const {
    if (HasBody()) return b2Body_GetGravityScale(body);
    return gravity_scale;
}

Vector2 Rigidbody::GetUpDirection() const {
    const float angle = HasBody() ? RadiansOf(b2Body_GetRotation(body))
                                  : ClockwiseDegreesToBox2DRadians(rotation);
    Vector2 result(std::sin(angle), -std::cos(angle));
    result.Normalize();
    return result;
}

Vector2 Rigidbody::GetRightDirection() const {
    const float angle = HasBody() ? RadiansOf(b2Body_GetRotation(body))
                                  : ClockwiseDegreesToBox2DRadians(rotation);
    Vector2 result(std::cos(angle), std::sin(angle));
    result.Normalize();
    return result;
}

void Rigidbody::ApplyImpulse(const Vector2 &impulse) {
    FiniteVector(impulse);
    if (!HasBody() || b2Body_GetType(body) != b2_dynamicBody) return;
    const double mass = b2Body_GetMass(body);
    const Vector2 velocity = FromBox2D(b2Body_GetLinearVelocity(body));
    const double gain = mass > 0 ? 1.0 / mass : 0.0;
    const Vector2 predicted(static_cast<float>(velocity.x + gain * impulse.x), static_cast<float>(velocity.y + gain * impulse.y));
    SquaredVector(velocity.x + gain * impulse.x, velocity.y + gain * impulse.y, "impulse would exceed finite velocity norms");
    PointSpeed(body, predicted, b2Body_GetAngularVelocity(body));
    b2Body_ApplyLinearImpulseToCenter(body, ToBox2D(impulse), true);
}

void Rigidbody::AddTorque(float torque) {
    Require(std::isfinite(torque) && std::abs(torque) <= kFloatLimit, "torque must be finite");
    if (!HasBody() || b2Body_GetType(body) != b2_dynamicBody) return;
    b2Body_ApplyTorque(body, torque, true);
}

float Rigidbody::GetMass() const {
    return HasBody() ? b2Body_GetMass(body) : 0.0f;
}
