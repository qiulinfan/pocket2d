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
void SpriteRenderer::ClearAnimationPose(std::uint64_t owner) {
    if (animation_owner_ != owner) return;
    animation_pose_.reset();
    animation_owner_ = 0;
}

void SpriteRenderer::SetSpriteCell(int row, int column) {
    sprite_row = std::max(row, 1);
    sprite_column = std::max(column, 1);
}

void SpriteRenderer::QueueDraw(float world_x, float world_y, float rotation_degrees) const {
    if (!enabled || actor == nullptr) return;
    const std::string &image = animation_pose_ ? animation_pose_->image : sprite;
    if (image.empty()) return;

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
        const std::uint64_t revision = SpritesheetConfig::Revision();
        const auto &subdirectory = Scene::GetActiveSceneSubdirectory().native();
        if (grid_cache_.revision != revision || grid_cache_.sprite != sprite ||
            grid_cache_.subdirectory != subdirectory) {
            const SpritesheetGridSpec grid_spec =
                SpritesheetConfig::ReadForImageResource(sprite, Scene::GetActiveSceneSubdirectory());
            grid_cache_ = {revision, sprite, subdirectory, std::max(grid_spec.rows, 1),
                           std::max(grid_spec.columns, 1)};
        }
        request.spritesheet_rows = grid_cache_.rows;
        request.spritesheet_columns = grid_cache_.columns;
        request.sprite_row = std::max(sprite_row, 1);
        request.sprite_column = std::max(sprite_column, 1);
    }
    Renderer::QueueSceneImageDraw(request);
}
