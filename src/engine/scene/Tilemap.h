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
#include <memory>
#include <optional>
#include <vector>

class LiquidGrid;

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

    Tilemap();
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
    // Committed IDs of a rectangle inside the map, row-major (bulk reads for
    // game-side mirrors and planners). Raises when the rectangle leaves the map.
    // Tile lighting for a cell rectangle (it may extend past the map): white
    // sky light enters cells with no solid tile and no ground tile (or an
    // open-ground terrain), coloured sources add light, and each channel is
    // multiplied by air_decay or solid_decay for every cell it leaves, so it
    // fades out gradually as in Terraria. The colour of each cell, at least
    // ambient, is written to a dynamic image of width x height pixels, one per
    // cell, drawn by name: it multiplies what is drawn under it, and smooth
    // interpolates between cell centres. Above the map is open sky.
    struct LightOptions {
        std::string image;
        float sky = 1.0f;
        float air_decay = 0.91f;
        float solid_decay = 0.56f;
        float ambient = 0.0f;
        std::vector<std::string> open_ground;
        std::vector<float> sources;  // x, y (cells) and r, g, b levels
        bool smooth = true;
    };
    // Returns the light it drew: r, g, b per cell, row-major, before ambient.
    std::vector<float> ComputeLight(int x, int y, int width, int height, const LightOptions &options) const;
    // A map picture of a cell rectangle (it may extend past the map), one pixel
    // per `step` cells: the colour of the topmost terrain there (solid, then
    // ground, then extra layers), else `sky` above row cave_y and `cave` below
    // it. Colours are by terrain name (RGB; a terrain without one counts as
    // empty). Written to a dynamic image of ceil(width/step) x ceil(height/step).
    struct MapOptions {
        std::string image;
        std::map<std::string, std::array<std::uint8_t, 3>> colors;
        std::array<std::uint8_t, 4> sky{0, 0, 0, 0};
        std::array<std::uint8_t, 4> cave{0, 0, 0, 0};
        int cave_y = 0;
        int step = 1;
        // When set, cells never revealed (RevealMap) are drawn in this colour.
        std::optional<std::array<std::uint8_t, 4>> fog;
    };
    void ComputeMap(int x, int y, int width, int height, const MapOptions &options) const;
    // Map exploration: cells within `radius` of a cell become revealed (a
    // Terraria-style fog of war). The state saves as text: run lengths of
    // alternating hidden and revealed cells in row-major order ("12,5,300").
    void RevealMap(int x, int y, int radius);
    bool IsRevealed(int x, int y) const;
    std::string SaveRevealed() const;
    void LoadRevealed(const std::string &runs);
    // Flowing liquids (scene/LiquidGrid.h) over the map's cells, created on
    // first use: per cell a kind ("water", "lava", "honey" or "") and an amount
    // 0..255. Cell edits on any layer wake the liquid around them.
    LiquidGrid &Liquid();
    const LiquidGrid *GetLiquidGrid() const { return liquid_.get(); }
    struct LiquidStepOptions {
        std::string solid = "solid";   // the layer whose nonzero cells block liquid (pending edits count)
        std::string harden;            // terrain placed on `solid` where water meets lava ("" leaves the cell empty)
        std::size_t budget = 20000;    // active cells processed per step
    };
    // One step of flow; returns the cells where water hardened lava (already
    // set to `harden`, so callers can mirror them).
    std::vector<std::pair<int, int>> StepLiquid(const LiquidStepOptions &options);
    // Turns cells of `layer` whose terrain is a key of `kinds` into full
    // liquid of the mapped kind and clears the terrain; returns the count.
    int LiquidFromTerrain(const std::string &layer, const std::map<std::string, std::string> &kinds);
    // Liquid of a cell rectangle (it may extend past the map) into a dynamic
    // image of `resolution` pixels per cell side: each cell is filled from its
    // bottom by its amount (full when liquid lies above it), coloured by kind.
    struct LiquidImageOptions {
        std::string image;
        std::map<std::string, std::array<std::uint8_t, 4>> colors;
        int resolution = 8;
    };
    void ComputeLiquid(int x, int y, int width, int height, const LiquidImageOptions &options) const;
    // Whether a world point lies in a committed nonzero solid cell of this
    // enabled map (the outline-only collision has no interior to query).
    bool IsSolidAt(double world_x, double world_y) const;
    // Whether IsSolidAt can be true anywhere: enabled, with a solid cell.
    bool HasSolidCells() const;
    // Layer names in index order: ground, solid, then the extra layers.
    std::vector<std::string> GetLayerNames() const;
    bool HasLayer(const std::string &layer) const;
    std::vector<int> GetCells(const std::string &layer, double x, double y, double width, double height) const;
    // Autotiling: the terrain name at a cell ("" when none; nullopt outside),
    // and placing/clearing ("") a terrain, which chooses the cell's tile from
    // its neighbours and re-chooses the four neighbours. Pending edits count.
    std::optional<std::string> GetTerrain(const std::string &layer, double x, double y) const;
    bool SetTerrain(const std::string &layer, double x, double y, const std::string &terrain);
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
    // Number of collision outlines (closed chains) in the current physics world.
    std::size_t GetFixtureCount() const;

private:
    struct TileRecord { int x, y, id; };
    struct Chunk {
        std::vector<std::vector<TileRecord>> tiles;  // one list per layer
        b2BodyId body = b2_nullBodyId;
        // Closed collision outlines (Box2D chains) on body.
        std::size_t outlines = 0;
        bool collision_dirty = true;
    };
    TilemapAsset::Data data_;
    std::optional<TilemapAsset::Data> replacement_;
    std::vector<std::uint8_t> revealed_;   // per cell, allocated on first reveal
    std::unique_ptr<LiquidGrid> liquid_;   // allocated on first use
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
    int EffectiveCell(int layer, int x, int y) const;
    void ReframeTerrainCell(int layer, int x, int y, const std::vector<int> &terrain_of);
    void ValidatePlacement(float x, float y, float rotation) const;
    int LayerIndex(const std::string &layer) const;
    static void RequireInteger(double value, const char *label);
    static std::vector<TileRecord> BuildTiles(const TilemapAsset::Data &data, int layer,
                                             int chunk_x, int chunk_y);
    bool OwnsLiveBodies() const;
    static void DestroyChunkBody(b2BodyId &body);
    void DestroyBody(b2BodyId &body);
    void ClearBodies();
};

#endif
