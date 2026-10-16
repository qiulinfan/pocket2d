#ifndef SPRITE_RENDERER_COMPONENT_H
#define SPRITE_RENDERER_COMPONENT_H

#include "scene/ActorHandle.h"
#include <filesystem>
#include <string>
#include <optional>
#include <cstdint>

struct Actor;

class SpriteRenderer {
public:
    SpriteRenderer() = default;
    SpriteRenderer(const SpriteRenderer &other);
    struct AnimationPose {
        std::string image;
        int columns = 1, rows = 1, row = 1, column = 1;
    };
    void SetAnimationPose(std::uint64_t owner, const AnimationPose &pose);
    // Updates the pose in place, reusing the stored image string.
    void SetAnimationPose(std::uint64_t owner, const std::string &image, int columns, int rows, int row, int column);
    void ClearAnimationPose(std::uint64_t owner);
    bool HasAnimationOverride() const { return animation_pose_.has_value(); }
    std::uint64_t GetAnimationOverrideOwner() const { return animation_owner_; }

    Actor *actor = nullptr;
    ActorHandle actor_handle;
    std::string key = "";
    bool enabled = true;

    std::string sprite = "";
    int r = 255;
    int g = 255;
    int b = 255;
    int a = 255;
    float pivot_x = 0.5f;
    float pivot_y = 0.5f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    int sprite_row = 1;
    int sprite_column = 1;
    int sorting_order = 0;
    bool auto_sorting_order = false;

    // Convenience API for Lua/gameplay code that wants to select one spritesheet
    // cell without assigning the two indices separately.
    void SetSpriteCell(int row, int column);

    // Queue one world-space draw at the owning actor's world pose. The caller,
    // the World's render-phase loop, has already established that this is the
    // actor's live component and resolved the pose from Transform or
    // Rigidbody. This runs during the render phase, so editor frozen frames
    // still show sprites.
    void QueueDraw(float world_x, float world_y, float rotation_degrees) const;

private:
    std::optional<AnimationPose> animation_pose_;
    std::uint64_t animation_owner_ = 0;

    // Spritesheet grid for `sprite`, valid while the metadata revision, the
    // sprite name and the scene subdirectory are unchanged.
    struct GridCache {
        std::uint64_t revision = 0;
        std::string sprite;
        std::filesystem::path::string_type subdirectory;
        int rows = 1;
        int columns = 1;
    };
    mutable GridCache grid_cache_;
};

#endif
