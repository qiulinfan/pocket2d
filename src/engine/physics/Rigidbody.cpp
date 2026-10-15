#include "scripting/LuaBoundary.h"
#include "physics/Rigidbody.h"
#include "physics/PhysicsMutationGuard.h"
#include "scene/Actor.h"
#include "scripting/ComponentManager.h"
#include "physics/RayCast.h"
#include "glm/glm.hpp"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

constexpr float kPhysicsSyncEpsilon = 0.0001f;
constexpr double kFloatLimit = static_cast<double>(std::numeric_limits<float>::max()) / 16.0;
constexpr double kSquaredLimit = static_cast<double>(std::numeric_limits<float>::max()) / 256.0;
constexpr float kStep = 1.0f / 60.0f;
std::uint64_t g_force_step = 0;

void Require(bool valid,const char *message) {
    if(!valid) throw std::invalid_argument(std::string("Rigidbody: ")+message);
}
void FiniteVector(const b2Vec2 &value) {
    Require(std::isfinite(value.x)&&std::isfinite(value.y),"vector components must be finite");
}
void SquaredVector(double x,double y,const char *message) {
    Require(std::isfinite(x)&&std::isfinite(y)&&x*x+y*y<=kSquaredLimit,message);
}
void Position(const b2Vec2 &position) {
    FiniteVector(position);
    Require(std::abs(position.x)<=1e8f&&std::abs(position.y)<=1e8f,"position must remain within +/-1e8");
}
void Unlocked(const b2Body *body) {
    if(body&&body->GetWorld()->IsLocked())
        throw std::runtime_error("Rigidbody transform/type mutation cannot run while physics is stepping");
}
void PositiveMass(double value,const char *message) {
    // Box2D stores both a quantity and its reciprocal in float. Positive
    // subnormal mass/inertia is not safe merely because its upper bound fits.
    Require(std::isfinite(value)&&value>=1.0/kFloatLimit&&value<=kFloatLimit,message);
}
struct Mass { double mass=0,inertia=0; };
Mass ShapeMass(float width,float height,float radius,bool circle,float density) {
    const double area=circle?static_cast<double>(b2_pi)*radius*radius:static_cast<double>(width)*height;
    const double expected_mass=static_cast<double>(density)*area;
    const double expected_inertia=expected_mass*(circle?0.5*radius*radius:(static_cast<double>(width)*width+static_cast<double>(height)*height)/12.0);
    if(density>0) {
        PositiveMass(expected_mass,"fixture mass or its reciprocal exceeds supported float range");
        PositiveMass(expected_inertia,"fixture inertia or its reciprocal exceeds supported float range");
    }
    b2MassData actual;
    if(circle) { b2CircleShape shape;shape.m_radius=radius;shape.ComputeMass(&actual,density); }
    else { b2PolygonShape shape;shape.SetAsBox(width*0.5f,height*0.5f);shape.ComputeMass(&actual,density); }
    Require(std::isfinite(actual.mass)&&std::isfinite(actual.I)&&actual.center.IsValid(),"derived fixture mass/center/inertia is not finite");
    if(density>0) { PositiveMass(actual.mass,"float fixture mass has an unsafe reciprocal");PositiveMass(actual.I,"float fixture inertia has an unsafe reciprocal"); }
    return {actual.mass,actual.I};
}
b2Vec2 Gravity() { return RayCast::GetPhysicsWorld()?RayCast::GetPhysicsWorld()->GetGravity():b2Vec2(0,9.8f); }
b2Vec2 Integration(double mass,double scale,const b2Vec2 &gravity,double force_x,double force_y,const b2Vec2 &velocity) {
    FiniteVector(gravity);FiniteVector(velocity);
    Require(std::isfinite(scale),"gravity scale must be finite");
    SquaredVector(scale*gravity.x,scale*gravity.y,"derived gravity acceleration exceeds finite squared-magnitude range");
    if(mass>0) PositiveMass(mass,"live mass has an unsafe reciprocal");
    Require(std::isfinite(mass)&&mass>=0,"live mass is invalid");
    const double scaled_mass=scale*mass;
    // This bundled Box2D integrates h*invMass*(gravityScale*mass*gravity+force).
    // Validate its real intermediate products, not only the simplified result.
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
struct Geometry { double extent=0,friction=0,restitution=0; };
Geometry Describe(const b2Body *body) {
    Geometry result;
    for(const auto *fixture=body->GetFixtureList();fixture;fixture=fixture->GetNext()) {
        const auto *shape=fixture->GetShape();
        if(shape->GetType()==b2Shape::e_circle) {
            const auto *circle=static_cast<const b2CircleShape *>(shape);
            result.extent=std::max(result.extent,std::hypot(static_cast<double>(circle->m_p.x),circle->m_p.y)+circle->m_radius);
        } else if(shape->GetType()==b2Shape::e_polygon) {
            const auto *polygon=static_cast<const b2PolygonShape *>(shape);
            for(int i=0;i<polygon->m_count;++i) result.extent=std::max(result.extent,std::hypot(static_cast<double>(polygon->m_vertices[i].x),polygon->m_vertices[i].y)+polygon->m_radius);
        }
        result.friction=std::max(result.friction,static_cast<double>(fixture->GetFriction()));
        result.restitution=std::max(result.restitution,static_cast<double>(fixture->GetRestitution()));
    }
    return result;
}
void Motion(double mass,double inertia,const Geometry &geometry,const b2Vec2 &velocity,double angular_velocity) {
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
void PointSpeed(const b2Body *body,const b2Vec2 &velocity,double angular_velocity) {
    Motion(body->GetMass(),body->GetInertia(),Describe(body),velocity,angular_velocity);
}
Mass ActualMass(const b2Body *body) {
    Mass total;
    double weighted_x=0,weighted_y=0;
    for(const auto *fixture=body->GetFixtureList();fixture;fixture=fixture->GetNext()) {
        Require(std::isfinite(fixture->GetDensity())&&fixture->GetDensity()>=0,"live fixture density is invalid");
        if(fixture->GetDensity()==0) continue;
        b2MassData data;fixture->GetMassData(&data);
        PositiveMass(data.mass,"live fixture mass has an unsafe reciprocal");
        PositiveMass(data.I,"live fixture inertia has an unsafe reciprocal");
        Require(data.center.IsValid(),"live fixture center is not finite");
        weighted_x+=static_cast<double>(data.mass)*data.center.x;
        weighted_y+=static_cast<double>(data.mass)*data.center.y;
        Require(std::isfinite(weighted_x)&&std::isfinite(weighted_y)&&std::abs(weighted_x)<=kFloatLimit&&std::abs(weighted_y)<=kFloatLimit,
                "live mass-weighted center exceeds supported float range");
        total.mass+=data.mass;total.inertia+=data.I;
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

class RigidbodyContactListener : public b2ContactListener {
public:
    void BeginContact(b2Contact *contact) override {
        QueueContactEvents(contact, true);
    }

    void EndContact(b2Contact *contact) override {
        QueueContactEvents(contact, false);
    }

private:
    static void QueueContactEvents(b2Contact *contact, bool is_enter) {
        if (contact == nullptr) return;

        b2Fixture *fixture_a = contact->GetFixtureA();
        b2Fixture *fixture_b = contact->GetFixtureB();
        if (fixture_a == nullptr || fixture_b == nullptr) return;

        const RayCast::FixtureMetadata *metadata_a = RayCast::FindFixtureMetadata(fixture_a);
        const RayCast::FixtureMetadata *metadata_b = RayCast::FindFixtureMetadata(fixture_b);
        if (metadata_a == nullptr || metadata_b == nullptr) return;
        if (metadata_a->actor == nullptr || metadata_b->actor == nullptr) return;

        const b2Vec2 velocity_a = fixture_a->GetBody()->GetLinearVelocity();
        const b2Vec2 velocity_b = fixture_b->GetBody()->GetLinearVelocity();
        const b2Vec2 relative_velocity = velocity_a - velocity_b;

        if (metadata_a->kind == RayCast::FixtureKind::Collider &&
            metadata_b->kind == RayCast::FixtureKind::Collider) {
            b2Vec2 point(-999.0f, -999.0f);
            b2Vec2 normal(-999.0f, -999.0f);
            if (is_enter) {
                b2WorldManifold world_manifold;
                contact->GetWorldManifold(&world_manifold);
                point = world_manifold.points[0];
                normal = world_manifold.normal;
            }

            ComponentManager::QueueCollisionEvent(
                metadata_a->actor, metadata_b->actor, point, relative_velocity,
                normal, is_enter);
            ComponentManager::QueueCollisionEvent(
                metadata_b->actor, metadata_a->actor, point, relative_velocity,
                normal, is_enter);
            return;
        }

        if (metadata_a->kind == RayCast::FixtureKind::Trigger &&
            metadata_b->kind == RayCast::FixtureKind::Trigger) {
            const b2Vec2 sentinel(-999.0f, -999.0f);
            ComponentManager::QueueTriggerEvent(
                metadata_a->actor, metadata_b->actor, sentinel, relative_velocity,
                sentinel, is_enter);
            ComponentManager::QueueTriggerEvent(
                metadata_b->actor, metadata_a->actor, sentinel, relative_velocity,
                sentinel, is_enter);
        }
    }
};

RigidbodyContactListener g_contact_listener;

float ClockwiseDegreesToBox2DRadians(float rotation_degrees) {
    return rotation_degrees * (b2_pi / 180.0f);
}

float Box2DRadiansToClockwiseDegrees(float rotation_radians) {
    return rotation_radians * (180.0f / b2_pi);
}

b2BodyType ParseBodyType(const std::string &body_type) {
    if (body_type == "static") return b2_staticBody;
    if (body_type == "kinematic") return b2_kinematicBody;
    if(body_type=="dynamic") return b2_dynamicBody;
    throw std::invalid_argument("Rigidbody body type must be dynamic/static/kinematic");
}

void CreateColliderFixture(Rigidbody &rigidbody) {
    if (rigidbody.body == nullptr || !rigidbody.has_collider) return;

    b2FixtureDef fixture_def;
    fixture_def.isSensor = false;
    fixture_def.density = rigidbody.density;
    fixture_def.friction = rigidbody.friction;
    fixture_def.restitution = rigidbody.bounciness;

    if (rigidbody.collider_type == "circle") {
        b2CircleShape shape;
        shape.m_radius = rigidbody.radius;
        fixture_def.shape = &shape;
        b2Fixture *fixture = rigidbody.body->CreateFixture(&fixture_def);
        RayCast::RegisterFixture(fixture, rigidbody.actor,
                                 RayCast::FixtureKind::Collider);
        return;
    }

    b2PolygonShape shape;
    shape.SetAsBox(rigidbody.width * 0.5f, rigidbody.height * 0.5f);
    fixture_def.shape = &shape;
    b2Fixture *fixture = rigidbody.body->CreateFixture(&fixture_def);
    RayCast::RegisterFixture(fixture, rigidbody.actor,
                             RayCast::FixtureKind::Collider);
}


void CreateTriggerFixture(Rigidbody &rigidbody) {
    if (rigidbody.body == nullptr || !rigidbody.has_trigger) return;

    b2FixtureDef trigger_fixture_def;
    trigger_fixture_def.isSensor = true;
    trigger_fixture_def.density = rigidbody.density;

    if (rigidbody.trigger_type == "circle") {
        b2CircleShape shape;
        shape.m_radius = rigidbody.trigger_radius;
        trigger_fixture_def.shape = &shape;
        b2Fixture *fixture = rigidbody.body->CreateFixture(&trigger_fixture_def);
        RayCast::RegisterFixture(fixture, rigidbody.actor,
                                 RayCast::FixtureKind::Trigger);
        return;
    }

    b2PolygonShape shape;
    shape.SetAsBox(rigidbody.trigger_width * 0.5f,
                   rigidbody.trigger_height * 0.5f);
    trigger_fixture_def.shape = &shape;
    b2Fixture *fixture = rigidbody.body->CreateFixture(&trigger_fixture_def);
    RayCast::RegisterFixture(fixture, rigidbody.actor,
                             RayCast::FixtureKind::Trigger);
}

void CreatePhantomFixture(Rigidbody &rigidbody) {
    if (rigidbody.body == nullptr || rigidbody.has_collider || rigidbody.has_trigger) {
        return;
    }

    b2PolygonShape phantom_shape;
    phantom_shape.SetAsBox(rigidbody.width * 0.5f, rigidbody.height * 0.5f);

    b2FixtureDef phantom_fixture_def;
    phantom_fixture_def.shape = &phantom_shape;
    phantom_fixture_def.density = rigidbody.density;
    phantom_fixture_def.isSensor = true;
    b2Fixture *fixture = rigidbody.body->CreateFixture(&phantom_fixture_def);
    RayCast::RegisterFixture(fixture, rigidbody.actor,
                             RayCast::FixtureKind::Phantom);
}

} // namespace

float Rigidbody::ValidateNumericProperty(const char *name,double value) {
    Require(std::isfinite(value)&&std::abs(value)<=std::numeric_limits<float>::max(),"numeric value must be finite and representable as float");
    const std::string_view field(name);
    if(field=="x"||field=="y") Require(std::abs(value)<=1e8,"position must remain within +/-1e8");
    else if(field=="width"||field=="height"||field=="radius"||field=="trigger_width"||field=="trigger_height"||field=="trigger_radius")
        Require(value>=0.001&&value<=1e6,"dimensions must be 0.001..1000000");
    else if(field=="density"||field=="angular_friction") Require(value>=0&&value<=kFloatLimit,"density/damping must be nonnegative with finite arithmetic headroom");
    else if(field=="friction"||field=="bounciness")
        Require(value>=0&&value*value<=kSquaredLimit,"friction/restitution must be nonnegative with finite product headroom");
    else if(field=="gravity_scale") SquaredVector(0,value*static_cast<double>(9.8f),"gravity scale exceeds finite acceleration norms");
    else if(field=="rotation") Require(std::abs(value)<=kFloatLimit,"rotation must retain finite conversion headroom");
    else throw std::invalid_argument("unknown Rigidbody numeric property");
    const float narrowed=static_cast<float>(value);
    Require(value==0||narrowed!=0,"numeric value underflows float range");
    return narrowed;
}
void Rigidbody::ValidateConfiguration() const {
    ParseBodyType(body_type);
    if(!effective_body_type.empty()&&effective_body_type!="none") ParseBodyType(effective_body_type);
    Require((collider_type=="box"||collider_type=="circle")&&(trigger_type=="box"||trigger_type=="circle"),"shapes must be box/circle");
    for(const auto &[name,value]:std::initializer_list<std::pair<const char *,float>>{
        {"x",x},{"y",y},{"rotation",rotation},{"gravity_scale",gravity_scale},{"density",density},{"angular_friction",angular_friction},
        {"width",width},{"height",height},{"radius",radius},{"trigger_width",trigger_width},{"trigger_height",trigger_height},
        {"trigger_radius",trigger_radius},{"friction",friction},{"bounciness",bounciness}}) ValidateNumericProperty(name,value);
    Mass total;
    const auto add=[&](Mass value){total.mass+=value.mass;total.inertia+=value.inertia;
        if(total.mass>0) PositiveMass(total.mass,"combined body mass has unsafe range/reciprocal");
        if(total.inertia>0) PositiveMass(total.inertia,"combined body inertia has unsafe range/reciprocal");};
    if(has_collider) add(ShapeMass(width,height,radius,collider_type=="circle",density));
    if(has_trigger) add(ShapeMass(trigger_width,trigger_height,trigger_radius,trigger_type=="circle",density));
    if(!has_collider&&!has_trigger) add(ShapeMass(width,height,radius,false,density));
    const auto predicted=Integration(total.mass,gravity_scale,Gravity(),0,0,b2Vec2_zero);
    Geometry geometry;
    geometry.friction=has_collider?friction:0;
    geometry.restitution=has_collider?bounciness:0;
    if(has_collider) geometry.extent=collider_type=="circle"?radius:std::hypot(static_cast<double>(width),height)*0.5+b2_polygonRadius;
    if(has_trigger) geometry.extent=std::max(geometry.extent,trigger_type=="circle"?static_cast<double>(trigger_radius):std::hypot(static_cast<double>(trigger_width),trigger_height)*0.5+b2_polygonRadius);
    if(!has_collider&&!has_trigger) geometry.extent=std::hypot(static_cast<double>(width),height)*0.5+b2_polygonRadius;
    Motion(total.mass,total.inertia,geometry,predicted,0);
}

// global physics world lifecycle
void Rigidbody::EnsurePhysicsWorld() {
    if (RayCast::GetPhysicsWorld() != nullptr) return;
    b2World *world = new b2World(b2Vec2(0.0f, 9.8f));
    world->SetContactListener(&g_contact_listener);
    RayCast::SetPhysicsWorld(world);
}

// global physics world lifecycle
void Rigidbody::DestroyPhysicsWorld() {
    delete RayCast::GetPhysicsWorld();
    RayCast::ClearPhysicsState();
    ++g_force_step;
}

// global physics world lifecycle
bool Rigidbody::HasPhysicsWorld() {
    return RayCast::GetPhysicsWorld() != nullptr;
}

// global physics world lifecycle
void Rigidbody::StepPhysicsWorld() {
    b2World *world = RayCast::GetPhysicsWorld();
    if (world == nullptr) return;
    world->Step(kStep, 8, 3);
    if(world->GetAutoClearForces()) ++g_force_step;
}

bool Rigidbody::HasLiveRuntimeBinding() const {
    if (actor_handle.generation == 0) return true;
    if (!actor_handle.IsValid()) return false;
    const auto current = ComponentManager::GetComponentByKey(actor_handle.uid, key);
    return current.isInstance<Rigidbody>() && LuaBoundary::Cast<Rigidbody *>(current) == this;
}

// Create the Box2D body and fixtures from the serialized runtime config.
void Rigidbody::OnStart() {
    // A Lua reference may outlive its owning actor. Lifecycle methods remain
    // callable from scripts, but must never recreate fixtures with stale owners.
    if (!HasLiveRuntimeBinding()) return;
    if (body != nullptr) return;
    ValidateConfiguration();
    EnsurePhysicsWorld();
    if(RayCast::GetPhysicsWorld()->IsLocked()) throw std::runtime_error("Rigidbody cannot create a body during a physics callback");

    b2BodyDef body_def;
    body_def.type = ParseBodyType(GetEffectiveBodyType());
    body_def.position.Set(x, y);
    body_def.bullet = precise;
    body_def.gravityScale = gravity_scale;
    body_def.angularDamping = angular_friction;
    body_def.angle = ClockwiseDegreesToBox2DRadians(rotation);
    body_def.enabled = enabled;

    body = RayCast::GetPhysicsWorld()->CreateBody(&body_def);

    CreateColliderFixture(*this);
    CreateTriggerFixture(*this);
    CreatePhantomFixture(*this);
    accumulated_force_.SetZero();force_step_=g_force_step;
}

// Destroy the backing Box2D body and unregister all fixture metadata.
void Rigidbody::OnDestroy() {
    b2World *world = RayCast::GetPhysicsWorld();
    if (body == nullptr || world == nullptr) return;
    if (PhysicsMutationGuard::IsActive())
        throw std::runtime_error("Rigidbody.OnDestroy cannot run during a native contact mutation callback; use actor:RemoveComponent(body) or Actor.Destroy(actor) for deferred cleanup");
    if (world->IsLocked())
        throw std::runtime_error("Rigidbody.OnDestroy cannot run during a physics callback; use actor:RemoveComponent(body) or Actor.Destroy(actor) for deferred cleanup");

    for (b2Fixture *fixture = body->GetFixtureList(); fixture != nullptr;
         fixture = fixture->GetNext()) {
        RayCast::UnregisterFixture(fixture);
    }
    PhysicsMutationGuard mutation;
    world->DestroyBody(body);
    body = nullptr;
    accumulated_force_.SetZero();force_step_=g_force_step;
}

// runtime physics controls
b2Vec2 Rigidbody::GetPosition() const {
    if (body != nullptr) return body->GetPosition();
    return b2Vec2(x, y);
}

// runtime physics controls
float Rigidbody::GetRotation() const {
    if (body != nullptr) {
        return Box2DRadiansToClockwiseDegrees(body->GetAngle());
    }
    return rotation;
}

std::string Rigidbody::GetEffectiveBodyType() const {
    if (!effective_body_type.empty() && effective_body_type != "none") {
        return effective_body_type;
    }
    return body_type;
}

// runtime physics controls
void Rigidbody::AddForce(const b2Vec2 &force) {
    FiniteVector(force);
    if (body == nullptr) return;
    if(body->GetType()!=b2_dynamicBody) return;
    const auto previous=force_step_==g_force_step?accumulated_force_:b2Vec2_zero;
    const double next_x=static_cast<double>(previous.x)+force.x,next_y=static_cast<double>(previous.y)+force.y;
    const auto predicted=Integration(body->GetMass(),body->GetGravityScale(),body->GetWorld()->GetGravity(),next_x,next_y,body->GetLinearVelocity());
    PointSpeed(body,predicted,body->GetAngularVelocity());
    body->ApplyForceToCenter(force, true);
    accumulated_force_.Set(static_cast<float>(next_x),static_cast<float>(next_y));force_step_=g_force_step;
}

// runtime physics controls
void Rigidbody::SetVelocity(const b2Vec2 &velocity) {
    FiniteVector(velocity);SquaredVector(velocity.x,velocity.y,"linear velocity exceeds finite squared-magnitude range");
    if (body == nullptr) return;
    const auto force=force_step_==g_force_step?accumulated_force_:b2Vec2_zero;
    PointSpeed(body,velocity,body->GetAngularVelocity());
    const auto predicted=Integration(body->GetMass(),body->GetGravityScale(),body->GetWorld()->GetGravity(),force.x,force.y,velocity);
    PointSpeed(body,predicted,body->GetAngularVelocity());
    body->SetLinearVelocity(velocity);
}

// runtime physics controls
void Rigidbody::SetPosition(const b2Vec2 &position) {
    Position(position);
    const auto current=GetPosition();
    const bool same_position = NearlyEqual(current.x, position.x) && NearlyEqual(current.y, position.y);
    if(!same_position) Unlocked(body);
    x = position.x;
    y = position.y;

    if (body == nullptr) return;
    if (same_position) return;
    body->SetTransform(position, body->GetAngle());
}

// runtime physics controls
void Rigidbody::SetRotation(float degrees_clockwise) {
    ValidateNumericProperty("rotation",degrees_clockwise);
    const bool same_rotation = NearlyEqual(GetRotation(), degrees_clockwise);
    if(!same_rotation) Unlocked(body);
    rotation = degrees_clockwise;

    if (body == nullptr) return;
    if (same_rotation) return;
    body->SetTransform(body->GetPosition(),
                       ClockwiseDegreesToBox2DRadians(degrees_clockwise));
}

void Rigidbody::SetEffectiveBodyType(const std::string &type_name) {
    if(type_name.empty()||type_name=="none") { effective_body_type=type_name;return; }
    const b2BodyType requested_type=ParseBodyType(type_name);
    // The hierarchy bridge calls this every tick. Pending authoring fields do
    // not reconfigure existing fixtures, and a no-op must not validate them.
    if(!body||body->GetType()==requested_type) { effective_body_type=type_name;return; }
    Unlocked(body);
    const auto mass=ActualMass(body); // Retype consumes actual fixtures, not pending raw config.
    Position(body->GetPosition());
    ValidateNumericProperty("rotation",Box2DRadiansToClockwiseDegrees(body->GetAngle()));
    const auto velocity=requested_type==b2_staticBody?b2Vec2_zero:body->GetLinearVelocity();
    const double angular=requested_type==b2_staticBody?0:body->GetAngularVelocity();
    const auto predicted=Integration(requested_type==b2_dynamicBody?mass.mass:0,body->GetGravityScale(),body->GetWorld()->GetGravity(),0,0,velocity);
    Motion(requested_type==b2_dynamicBody?mass.mass:0,requested_type==b2_dynamicBody?mass.inertia:0,Describe(body),predicted,angular);
    PhysicsMutationGuard state_change;
    body->SetType(requested_type);
    effective_body_type = type_name;
    accumulated_force_.SetZero();force_step_=g_force_step;
}

void Rigidbody::ApplyEnabledState() {
    if (!body || body->IsEnabled() == enabled) return;
    Unlocked(body);
    PhysicsMutationGuard state_change;
    body->SetEnabled(enabled);
}


// runtime physics controls
void Rigidbody::SetAngularVelocity(float degrees_clockwise) {
    ValidateNumericProperty("rotation",degrees_clockwise);
    const auto radians=ClockwiseDegreesToBox2DRadians(degrees_clockwise);
    Require(static_cast<double>(radians)*radians<=kSquaredLimit,"angular velocity exceeds finite squared-magnitude range");
    if (body == nullptr) return;
    PointSpeed(body,body->GetLinearVelocity(),radians);
    body->SetAngularVelocity(radians);
}

// runtime physics controls
void Rigidbody::SetGravityScale(float scale) {
    ValidateNumericProperty("gravity_scale",scale);
    if(body) {
        const auto force=force_step_==g_force_step?accumulated_force_:b2Vec2_zero;
        const auto predicted=Integration(body->GetMass(),scale,body->GetWorld()->GetGravity(),force.x,force.y,body->GetLinearVelocity());
        PointSpeed(body,predicted,body->GetAngularVelocity());
    }
    gravity_scale = scale;
    if (body == nullptr) return;
    body->SetGravityScale(scale);
}

// runtime physics controls
void Rigidbody::SetUpDirection(b2Vec2 direction) {
    FiniteVector(direction);
    if(direction.x==0&&direction.y==0) return;
    Unlocked(body);
    const float new_angle_radians=static_cast<float>(std::atan2(static_cast<double>(direction.x),-static_cast<double>(direction.y)));
    rotation = Box2DRadiansToClockwiseDegrees(new_angle_radians);

    if (body == nullptr) return;
    body->SetTransform(body->GetPosition(), new_angle_radians);
}

// runtime physics controls
void Rigidbody::SetRightDirection(b2Vec2 direction) {
    FiniteVector(direction);
    if(direction.x==0&&direction.y==0) return;
    Unlocked(body);
    const float new_angle_radians=static_cast<float>(std::atan2(static_cast<double>(direction.x),-static_cast<double>(direction.y))-b2_pi/2.0);
    rotation = Box2DRadiansToClockwiseDegrees(new_angle_radians);

    if (body == nullptr) return;
    body->SetTransform(body->GetPosition(), new_angle_radians);
}

// runtime physics controls
b2Vec2 Rigidbody::GetVelocity() const {
    if (body != nullptr) return body->GetLinearVelocity();
    return b2Vec2(0.0f, 0.0f);
}

// runtime physics controls
float Rigidbody::GetAngularVelocity() const {
    if (body != nullptr) {
        return Box2DRadiansToClockwiseDegrees(body->GetAngularVelocity());
    }
    return 0.0f;
}

// runtime physics controls
float Rigidbody::GetGravityScale() const {
    if (body != nullptr) return body->GetGravityScale();
    return gravity_scale;
}

// runtime physics controls
b2Vec2 Rigidbody::GetUpDirection() const {
    const float angle = (body != nullptr) ? body->GetAngle()
                                          : ClockwiseDegreesToBox2DRadians(rotation);
    b2Vec2 result(glm::sin(angle), -glm::cos(angle));
    result.Normalize();
    return result;
}

// runtime physics controls
b2Vec2 Rigidbody::GetRightDirection() const {
    const float angle = (body != nullptr) ? body->GetAngle()
                                          : ClockwiseDegreesToBox2DRadians(rotation);
    b2Vec2 result(glm::cos(angle), glm::sin(angle));
    result.Normalize();
    return result;
}
