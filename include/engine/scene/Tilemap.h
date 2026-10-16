#ifndef POCKET2D_ENGINE_TILEMAP_H
#define POCKET2D_ENGINE_TILEMAP_H

#include "core/Vector2.h"
#include "scene/ActorHandle.h"
#include "shared/tilemap/TilemapAsset.h"
#include "rendering/Renderer.h"
#include "physics/PhysicsIds.h"
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

class Tilemap {
public:
    static constexpr int kChunkSize = 32;
    Actor *actor = nullptr;
    ActorHandle actor_handle;
    std::string key;
    bool enabled = true;
    std::string map_resource;

    struct Diagnostics {
        std::uint64_t render_chunk_rebuilds = 0;
        std::uint64_t collision_chunk_rebuilds = 0;
        std::uint64_t fixtures_created = 0;
        std::uint64_t visible_tiles = 0;
    };

    Tilemap() = default;
    Tilemap(const Tilemap &other);
    Tilemap &operator=(const Tilemap &) = delete;
    ~Tilemap();
    // Prepared data comes from TilemapAsset::LoadResource/LoadFile. This path
    // copies cell state and builds CPU records without file I/O or physics.
    static void ValidateInitialization(const TilemapAsset::Data &data, float origin_x, float origin_y);
    void Initialize(const TilemapAsset::Data &data, float origin_x = 0, float origin_y = 0);
    // Commit-only transfer from a fresh detached prepared value. Preserves this
    // component's identity; consumes prepared. No resource I/O or asset validation.
    void AdoptPreparedState(Tilemap &prepared);
    void ReplaceAsset(const TilemapAsset::Data &data);
    bool HasAsset() const;
    void OnStart();
    void OnDestroy();
    void CommitPending();
    void CommitPending(float origin_x, float origin_y, float rotation = 0);

    Vector2 WorldToCell(double world_x, double world_y) const;
    Vector2 GetCellCenter(double x, double y) const;
    bool InBounds(double x, double y) const;
    std::optional<int> GetCell(const std::string &layer, double x, double y) const;
    bool SetCell(const std::string &layer, double x, double y, double id);
    std::uint64_t GetRevision() const { return revision_; }
    int GetWidth() const { return data_.width; }
    int GetHeight() const { return data_.height; }
    float GetCellSize() const { return data_.cell_size; }
    const TilemapAsset::Data &GetAsset() const { return data_; }
    std::vector<ImageDrawRequest> CollectVisible(double min_x, double min_y,
                                                double max_x, double max_y);
    std::size_t QueueVisible(double min_x, double min_y, double max_x, double max_y);

    const Diagnostics &GetDiagnostics() const { return diagnostics_; }
    void ResetDiagnostics() { diagnostics_ = {}; }
    b2BodyId GetChunkBody(int chunk_x, int chunk_y) const;
    std::size_t GetFixtureCount() const;

private:
    struct TileRecord { int x, y, id; };
    struct RowRun { int x, y, length; };
    struct Chunk {
        std::array<std::vector<TileRecord>, 2> tiles;
        b2BodyId body = b2_nullBodyId;
        bool collision_dirty = true;
    };
    TilemapAsset::Data data_;
    std::optional<TilemapAsset::Data> replacement_;
    std::map<std::size_t, int> pending_;
    std::vector<Chunk> chunks_;
    int chunks_x_ = 0;
    int chunks_y_ = 0;
    float origin_x_ = 0;
    float origin_y_ = 0;
    std::uint64_t revision_ = 0;
    bool initialized_ = false;
    bool committed_enabled_ = true;
    bool destroyed_ = false;
    bool committing_ = false;
    b2WorldId physics_world_ = b2_nullWorldId;
    Diagnostics diagnostics_;

    bool HasLiveBinding() const;
    void ValidatePlacement(float x, float y, float rotation) const;
    static int LayerIndex(const std::string &layer);
    static void RequireInteger(double value, const char *label);
    static std::vector<TileRecord> BuildTiles(const TilemapAsset::Data &data, int layer,
                                             int chunk_x, int chunk_y);
    static std::vector<RowRun> BuildRuns(const TilemapAsset::Data &data,
                                        int chunk_x, int chunk_y);
    bool OwnsLiveBodies() const;
    static void DestroyChunkBody(b2BodyId &body);
    void DestroyBody(b2BodyId &body);
    void ClearBodies();
};

#endif
