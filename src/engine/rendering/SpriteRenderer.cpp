#include "scripting/LuaBoundary.h"
#include "rendering/SpriteRenderer.h"
#include "physics/Rigidbody.h"
#include "rendering/Renderer.h"
#include "scene/Actor.h"
#include "scene/Scene.h"
#include "scene/Transform.h"
#include "shared/config/SpritesheetConfig.h"
#include "scripting/ComponentManager.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>

namespace {

bool TryReadTransformState(const Actor *actor, float &out_x, float &out_y,
                           float &out_rotation) {
    if (actor == nullptr) return false;

    const luabridge::LuaRef transform_ref = actor->GetComponent("Transform");
    if (transform_ref.isNil()) return false;

    Transform *transform = nullptr;
    try {
        transform = LuaBoundary::Cast<Transform *>(transform_ref);
    } catch (const LuaBoundary::Error &) {
        return false;
    }
    if (transform == nullptr) return false;

    out_x = transform->world_x;
    out_y = transform->world_y;
    out_rotation = transform->world_rotation;
    return true;
}

bool TryReadRigidbodyState(const Actor *actor, float &out_x, float &out_y,
                           float &out_rotation) {
    if (actor == nullptr) return false;

    const luabridge::LuaRef rigidbody_ref = actor->GetComponent("Rigidbody");
    if (rigidbody_ref.isNil()) return false;

    Rigidbody *rigidbody = nullptr;
    try {
        rigidbody = LuaBoundary::Cast<Rigidbody *>(rigidbody_ref);
    } catch (const LuaBoundary::Error &) {
        return false;
    }
    if (rigidbody == nullptr) return false;

    const Vector2 position = rigidbody->GetPosition();
    out_x = position.x;
    out_y = position.y;
    out_rotation = rigidbody->GetRotation();
    return true;
}

} // namespace

SpriteRenderer::SpriteRenderer(const SpriteRenderer &other)
    : actor(other.actor), actor_handle(other.actor_handle), key(other.key), enabled(other.enabled),
      sprite(other.sprite), r(other.r), g(other.g), b(other.b), a(other.a),
      pivot_x(other.pivot_x), pivot_y(other.pivot_y), scale_x(other.scale_x), scale_y(other.scale_y),
      sprite_row(other.sprite_row), sprite_column(other.sprite_column),
      sorting_order(other.sorting_order), auto_sorting_order(other.auto_sorting_order) {}

void SpriteRenderer::SetAnimationPose(std::uint64_t owner, const AnimationPose &pose) {
    animation_pose_ = pose;
    animation_owner_ = owner;
}
void SpriteRenderer::ClearAnimationPose(std::uint64_t owner) {
    if (animation_owner_ != owner) return;
    animation_pose_.reset();
    animation_owner_ = 0;
}

void SpriteRenderer::SetSpriteCell(int row, int column) {
    sprite_row = std::max(row, 1);
    sprite_column = std::max(column, 1);
}

void SpriteRenderer::QueueDraw() const {
    if (!enabled || actor == nullptr) return;
    if (actor_handle.generation != 0) {
        if (!actor_handle.IsValid()) return;
        const auto reference = ComponentManager::GetComponentByKey(actor_handle.uid, key);
        if (!reference.isInstance<SpriteRenderer>() || LuaBoundary::Cast<SpriteRenderer *>(reference) != this) return;
    }
    const std::string &image = animation_pose_ ? animation_pose_->image : sprite;
    if (image.empty()) return;

    float world_x = 0.0f;
    float world_y = 0.0f;
    float rotation_degrees = 0.0f;

    // Prefer Transform when present so scene-authoring edits have one stable
    // source of truth. Fall back to Rigidbody for older gameplay scenes that
    // never authored a Transform component.
    if (!TryReadTransformState(actor, world_x, world_y, rotation_degrees) &&
        !TryReadRigidbodyState(actor, world_x, world_y, rotation_degrees)) {
        return;
    }

    ImageDrawRequest request;
    request.image_name = image;
    request.x = world_x;
    request.y = world_y;
    request.rotation_degrees = rotation_degrees;
    request.scale_x = scale_x;
    request.scale_y = scale_y;
    request.pivot_x = pivot_x;
    request.pivot_y = pivot_y;
    request.r = r;
    request.g = g;
    request.b = b;
    request.a = a;
    request.sorting_order = auto_sorting_order ? static_cast<int>(world_y) : sorting_order;
    if (animation_pose_) {
        request.spritesheet_rows = animation_pose_->rows;
        request.spritesheet_columns = animation_pose_->columns;
        request.sprite_row = animation_pose_->row;
        request.sprite_column = animation_pose_->column;
    } else {
        const SpritesheetGridSpec grid_spec =
            SpritesheetConfig::ReadForImageResource(sprite, Scene::GetActiveSceneSubdirectory());
        request.spritesheet_rows = std::max(grid_spec.rows, 1);
        request.spritesheet_columns = std::max(grid_spec.columns, 1);
        request.sprite_row = std::max(sprite_row, 1);
        request.sprite_column = std::max(sprite_column, 1);
    }
    Renderer::QueueSceneImageDraw(request);
}
