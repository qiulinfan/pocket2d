#include "rendering/SpriteRenderer.h"
#include "rendering/Renderer.h"
#include "scene/Actor.h"
#include "scene/Scene.h"
#include "shared/config/SpritesheetConfig.h"
#include <algorithm>

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
void SpriteRenderer::SetAnimationPose(std::uint64_t owner, const std::string &image, int columns, int rows,
                                      int row, int column) {
    if (!animation_pose_) animation_pose_.emplace();
    if (animation_pose_->image != image) animation_pose_->image = image;
    animation_pose_->columns = columns; animation_pose_->rows = rows;
    animation_pose_->row = row; animation_pose_->column = column;
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

SpriteRenderer::DrawContext SpriteRenderer::CurrentDrawContext() {
    return {SpritesheetConfig::Revision(), &Scene::GetActiveSceneSubdirectory()};
}

void SpriteRenderer::QueueDraw(float world_x, float world_y, float rotation_degrees) const {
    QueueDraw(world_x, world_y, rotation_degrees, CurrentDrawContext());
}

void SpriteRenderer::QueueDraw(float world_x, float world_y, float rotation_degrees,
                               const DrawContext &context) const {
    if (!enabled || actor == nullptr) return;
    const std::string &image = animation_pose_ ? animation_pose_->image : sprite;
    if (image.empty()) return;
    if (drawn_image_id_ == 0 || drawn_image_ != image) {
        drawn_image_ = image;
        drawn_image_id_ = Renderer::InternImage(image);
    }

    ImageDrawRequest request;
    request.image_id = drawn_image_id_;
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
    request.sorting_order = sorting_order;
    request.sort_y = auto_sorting_order ? world_y : 0.0f;
    request.interpolation_key = Renderer::ActorInterpolationKey(actor->uid);
    if (animation_pose_) {
        request.spritesheet_rows = animation_pose_->rows;
        request.spritesheet_columns = animation_pose_->columns;
        request.sprite_row = animation_pose_->row;
        request.sprite_column = animation_pose_->column;
    } else {
        const auto &subdirectory = context.scene_subdirectory->native();
        if (grid_cache_.revision != context.spritesheet_revision || grid_cache_.sprite != sprite ||
            grid_cache_.subdirectory != subdirectory) {
            const SpritesheetGridSpec grid_spec =
                SpritesheetConfig::ReadForImageResource(sprite, *context.scene_subdirectory);
            grid_cache_ = {context.spritesheet_revision, sprite, subdirectory, std::max(grid_spec.rows, 1),
                           std::max(grid_spec.columns, 1)};
        }
        request.spritesheet_rows = grid_cache_.rows;
        request.spritesheet_columns = grid_cache_.columns;
        request.sprite_row = std::max(sprite_row, 1);
        request.sprite_column = std::max(sprite_column, 1);
    }
    Renderer::QueueSceneImageDraw(request);
}
