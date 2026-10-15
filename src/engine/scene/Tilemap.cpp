#include "scripting/LuaBoundary.h"
#include "scene/Tilemap.h"
#include "physics/Rigidbody.h"
#include "physics/RayCast.h"
#include "scripting/ComponentManager.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
const TilemapAsset::Layer &LayerAt(const TilemapAsset::Data &data, int index) {
    return index == 0 ? data.ground : data.solid;
}
TilemapAsset::Layer &LayerAt(TilemapAsset::Data &data, int index) {
    return index == 0 ? data.ground : data.solid;
}
void FiniteFloat(double value, const char *label) {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        throw std::runtime_error(std::string("tilemap: ") + label + " must be finite and representable");
}
} // namespace

Tilemap::Tilemap(const Tilemap &other) : enabled(other.enabled), map_resource(other.map_resource) {
    if (other.HasAsset()) Initialize(other.data_, other.origin_x_, other.origin_y_);
}
Tilemap::~Tilemap() {
    // Runtime removal normally calls OnDestroy at its safe pre-physics boundary.
    // Native authoring/test instances also release their bodies when scoped out.
    if (RayCast::GetPhysicsWorld() == physics_world_ && physics_world_ != nullptr &&
        !physics_world_->IsLocked()) ClearBodies();
}
bool Tilemap::HasAsset() const { return initialized_ && !destroyed_; }
bool Tilemap::HasLiveBinding() const {
    if (destroyed_) return false;
    if (actor_handle.generation == 0) return true;
    if (!actor_handle.IsValid()) return false;
    const auto component = ComponentManager::GetComponentByKey(actor_handle.uid, key);
    return component.isInstance<Tilemap>() && LuaBoundary::Cast<Tilemap *>(component) == this;
}
int Tilemap::LayerIndex(const std::string &layer) {
    if (layer == "ground") return 0;
    if (layer == "solid") return 1;
    throw std::runtime_error("tilemap: unknown layer '" + layer + "'");
}
void Tilemap::RequireInteger(double value, const char *label) {
    if (!std::isfinite(value) || std::floor(value) != value)
        throw std::runtime_error(std::string("tilemap: ") + label + " must be a finite integer");
}
void Tilemap::ValidatePlacement(float x, float y, float rotation) const {
    FiniteFloat(x, "origin x"); FiniteFloat(y, "origin y");
    if (!std::isfinite(rotation) || rotation != 0)
        throw std::runtime_error("tilemap: rotation must be zero (actor " + std::to_string(actor_handle.uid) + ", key " + key + ")");
    const auto &asset = replacement_ ? *replacement_ : data_;
    FiniteFloat(static_cast<double>(x) + asset.width * static_cast<double>(asset.cell_size), "world extent x");
    FiniteFloat(static_cast<double>(y) + asset.height * static_cast<double>(asset.cell_size), "world extent y");
    if (actor_handle.generation != 0) {
        const Actor *owner = actor_handle.Resolve();
        if (owner != nullptr && owner->parent_uid != Actor::kInvalidUID)
            throw std::runtime_error("tilemap: actor must be root (actor " + std::to_string(actor_handle.uid) + ", key " + key + ")");
        if (!ComponentManager::GetComponentByType(actor_handle.uid, "Rigidbody").isNil())
            throw std::runtime_error("tilemap: owning actor cannot also have Rigidbody");
        if (ComponentManager::GetComponentsByType(actor_handle.uid, "Tilemap").length() > 1)
            throw std::runtime_error("tilemap: owning actor cannot have multiple Tilemaps");
    }
}
std::vector<Tilemap::TileRecord> Tilemap::BuildTiles(const TilemapAsset::Data &data,
                                                    int layer, int cx, int cy) {
    std::vector<TileRecord> records;
    for (int y = cy * kChunkSize; y < std::min((cy + 1) * kChunkSize, data.height); ++y)
        for (int x = cx * kChunkSize; x < std::min((cx + 1) * kChunkSize, data.width); ++x) {
            int id = LayerAt(data, layer).cells[static_cast<std::size_t>(y) * data.width + x];
            if (id != 0) records.push_back({x, y, id});
        }
    return records;
}
std::vector<Tilemap::RowRun> Tilemap::BuildRuns(const TilemapAsset::Data &data, int cx, int cy) {
    std::vector<RowRun> runs;
    const int end_x = std::min((cx + 1) * kChunkSize, data.width);
    for (int y = cy * kChunkSize; y < std::min((cy + 1) * kChunkSize, data.height); ++y) {
        int x = cx * kChunkSize;
        while (x < end_x) {
            if (data.solid.cells[static_cast<std::size_t>(y) * data.width + x] == 0) { ++x; continue; }
            const int start = x++;
            while (x < end_x && data.solid.cells[static_cast<std::size_t>(y) * data.width + x] != 0) ++x;
            runs.push_back({start, y, x - start});
        }
    }
    return runs;
}
void Tilemap::ValidateInitialization(const TilemapAsset::Data &data, float x, float y) {
    TilemapAsset::ValidateData(data);
    FiniteFloat(x, "origin x"); FiniteFloat(y, "origin y");
    FiniteFloat(static_cast<double>(x) + data.width * static_cast<double>(data.cell_size), "world extent x");
    FiniteFloat(static_cast<double>(y) + data.height * static_cast<double>(data.cell_size), "world extent y");
    FiniteFloat(static_cast<double>(data.cell_size) * 100 / data.atlas.tile_width_px, "tile render scale x");
    FiniteFloat(static_cast<double>(data.cell_size) * 100 / data.atlas.tile_height_px, "tile render scale y");
}
void Tilemap::Initialize(const TilemapAsset::Data &data, float x, float y) {
    ValidateInitialization(data, x, y);
    const int nx = (data.width + kChunkSize - 1) / kChunkSize;
    const int ny = (data.height + kChunkSize - 1) / kChunkSize;
    auto copy = data;
    std::vector<Chunk> chunks(static_cast<std::size_t>(nx) * ny);
    for (int cy = 0; cy < ny; ++cy)
        for (int cx = 0; cx < nx; ++cx)
            for (int layer = 0; layer < 2; ++layer)
                chunks[cy * nx + cx].tiles[layer] = BuildTiles(copy, layer, cx, cy);
    ClearBodies();
    data_ = std::move(copy);
    chunks_ = std::move(chunks);
    chunks_x_ = nx; chunks_y_ = ny;
    origin_x_ = x; origin_y_ = y;
    initialized_ = true; destroyed_ = false; revision_ = 0;
    committed_enabled_ = enabled;
    pending_.clear(); replacement_.reset();
    diagnostics_ = {};
    diagnostics_.render_chunk_rebuilds = chunks_.size() * 2;
}
void Tilemap::AdoptPreparedState(Tilemap &prepared) {
    if (&prepared == this || committing_ || !HasLiveBinding())
        throw std::runtime_error("tilemap: adoption requires a live component outside commit");
    if (prepared.actor != nullptr || prepared.actor_handle.generation != 0 || prepared.destroyed_ ||
        prepared.committing_ || prepared.physics_world_ != nullptr || !prepared.pending_.empty() || prepared.replacement_)
        throw std::runtime_error("tilemap: adoption requires a fresh detached prepared value");
    if (auto *world = RayCast::GetPhysicsWorld(); world && world->IsLocked())
        throw std::runtime_error("tilemap: adoption requires an unlocked world");
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("tilemap: revision exhausted");
    // This is a destructive commit, not rollback on callback/allocation errors.
    // Install the new base BEFORE EndContact can queue edits against it.
    struct CommitGuard { bool &value; CommitGuard(bool &flag) : value(flag) { value = true; } ~CommitGuard() { value = false; } } guard(committing_);
    auto old_chunks = std::move(chunks_);
    auto *old_world = physics_world_;
    pending_.clear(); replacement_.reset();
    data_ = std::move(prepared.data_);
    chunks_ = std::move(prepared.chunks_);
    chunks_x_ = prepared.chunks_x_; chunks_y_ = prepared.chunks_y_;
    origin_x_ = prepared.origin_x_; origin_y_ = prepared.origin_y_;
    initialized_ = prepared.initialized_;
    enabled = prepared.enabled; committed_enabled_ = enabled;
    map_resource = std::move(prepared.map_resource);
    physics_world_ = nullptr;
    ++revision_;
    diagnostics_ = prepared.diagnostics_;
    for (auto &chunk : chunks_) { chunk.body = nullptr; chunk.collision_dirty = true; }
    prepared.data_ = {};
    prepared.chunks_x_ = prepared.chunks_y_ = 0;
    prepared.pending_.clear(); prepared.replacement_.reset();
    prepared.initialized_ = false; prepared.destroyed_ = true;
    // Do not clear pending_/replacement_/enabled after this loop: callbacks may
    // have queued changes, which belong to the next CommitPending boundary.
    if (old_world && old_world == RayCast::GetPhysicsWorld()) {
        for (auto &chunk : old_chunks) {
            if (!chunk.body || old_world != RayCast::GetPhysicsWorld()) continue;
            for (auto *fixture = chunk.body->GetFixtureList(); fixture; fixture = fixture->GetNext())
                RayCast::UnregisterFixture(fixture);
            old_world->DestroyBody(chunk.body);
            chunk.body = nullptr;
        }
    }
}

void Tilemap::ReplaceAsset(const TilemapAsset::Data &data) {
    if (!HasLiveBinding()) throw std::runtime_error("tilemap: expired component");
    ValidateInitialization(data, origin_x_, origin_y_);
    auto prepared = data;
    replacement_ = std::move(prepared);
    pending_.clear();
}
void Tilemap::OnStart() {
    if (!HasAsset() || !HasLiveBinding()) return;
    ValidatePlacement(origin_x_, origin_y_, 0);
}
void Tilemap::DestroyBody(b2Body *body) {
    if (body == nullptr || physics_world_ == nullptr || RayCast::GetPhysicsWorld() != physics_world_) return;
    for (auto *fixture = body->GetFixtureList(); fixture != nullptr; fixture = fixture->GetNext())
        RayCast::UnregisterFixture(fixture);
    physics_world_->DestroyBody(body);
}
void Tilemap::ClearBodies() {
    if (physics_world_ != nullptr && RayCast::GetPhysicsWorld() == physics_world_ && physics_world_->IsLocked())
        throw std::runtime_error("tilemap: physics mutation requires an unlocked world");
    for (auto &chunk : chunks_) {
        DestroyBody(chunk.body);
        chunk.body = nullptr;
    }
    physics_world_ = nullptr;
}
void Tilemap::OnDestroy() {
    if (auto *world = RayCast::GetPhysicsWorld(); world != nullptr && world->IsLocked())
        throw std::runtime_error("Tilemap.OnDestroy cannot run during a physics callback; use actor:RemoveComponent(map) or Actor.Destroy(actor) for deferred cleanup");
    if (committing_) throw std::runtime_error("tilemap: cannot destroy during commit");
    ClearBodies();
    pending_.clear(); replacement_.reset();
    destroyed_ = true;
    committed_enabled_ = false;
}
bool Tilemap::InBounds(double x, double y) const {
    RequireInteger(x, "cell x"); RequireInteger(y, "cell y");
    return HasAsset() && x >= 0 && y >= 0 && x < data_.width && y < data_.height;
}
std::optional<int> Tilemap::GetCell(const std::string &layer, double x, double y) const {
    const int index = LayerIndex(layer);
    if (!InBounds(x, y) || !HasLiveBinding()) return std::nullopt;
    return LayerAt(data_, index).cells[static_cast<std::size_t>(y) * data_.width + static_cast<std::size_t>(x)];
}
bool Tilemap::SetCell(const std::string &layer, double x, double y, double id) {
    const int layer_index = LayerIndex(layer);
    RequireInteger(x, "cell x"); RequireInteger(y, "cell y"); RequireInteger(id, "tile ID");
    if (!HasLiveBinding() || (!HasAsset() && !replacement_)) return false;
    const auto &asset = replacement_ ? *replacement_ : data_;
    const auto maximum = static_cast<std::int64_t>(asset.atlas.columns) * asset.atlas.rows;
    if (id < 0 || id > maximum) throw std::runtime_error("tilemap: tile ID outside atlas range");
    if (x < 0 || y < 0 || x >= asset.width || y >= asset.height) return false;
    const auto count = static_cast<std::size_t>(asset.width) * asset.height;
    const auto cell = static_cast<std::size_t>(y) * asset.width + static_cast<std::size_t>(x);
    const auto command = layer_index * count + cell;
    const int original = LayerAt(asset, layer_index).cells[cell];
    const auto found = pending_.find(command);
    if ((found == pending_.end() ? original : found->second) == static_cast<int>(id)) return false;
    if (original == static_cast<int>(id)) pending_.erase(command);
    else pending_[command] = static_cast<int>(id);
    return true;
}
b2Vec2 Tilemap::WorldToCell(double x, double y) const {
    FiniteFloat(x, "world x"); FiniteFloat(y, "world y");
    const double cx = std::floor((x - origin_x_) / data_.cell_size);
    const double cy = std::floor((y - origin_y_) / data_.cell_size);
    FiniteFloat(cx, "converted cell x"); FiniteFloat(cy, "converted cell y");
    return {static_cast<float>(cx), static_cast<float>(cy)};
}
b2Vec2 Tilemap::GetCellCenter(double x, double y) const {
    RequireInteger(x, "cell x"); RequireInteger(y, "cell y");
    const double wx = origin_x_ + (x + 0.5) * data_.cell_size;
    const double wy = origin_y_ + (y + 0.5) * data_.cell_size;
    FiniteFloat(wx, "cell center x"); FiniteFloat(wy, "cell center y");
    return {static_cast<float>(wx), static_cast<float>(wy)};
}
void Tilemap::CommitPending() { CommitPending(origin_x_, origin_y_, 0); }
void Tilemap::CommitPending(float x, float y, float rotation) {
    if ((!HasAsset() && !replacement_) || !HasLiveBinding()) return;
    if (committing_) throw std::runtime_error("tilemap: recursive commit is not allowed");
    // EndContact may run while bodies are disabled and may queue another flag
    // change. Apply this one snapshot to every body and the rendered state.
    const bool next_enabled = enabled;
    ValidatePlacement(x, y, rotation);
    if (auto *world = RayCast::GetPhysicsWorld(); world != nullptr && world->IsLocked())
        throw std::runtime_error("tilemap: commit requires pre-physics safe boundary");
    if (physics_world_ != nullptr && physics_world_ != RayCast::GetPhysicsWorld()) {
        for (auto &chunk : chunks_) { chunk.body = nullptr; chunk.collision_dirty = true; }
        physics_world_ = nullptr;
    }
    const auto &candidate = replacement_ ? *replacement_ : data_;
    const int nx = (candidate.width + kChunkSize - 1) / kChunkSize;
    const int ny = (candidate.height + kChunkSize - 1) / kChunkSize;
    const auto cell_count = static_cast<std::size_t>(candidate.width) * candidate.height;
    struct Prepared {
        std::size_t index;
        unsigned mask;
        bool collision;
        std::array<std::vector<TileRecord>, 2> tiles;
        std::vector<RowRun> runs;
        b2Body *body = nullptr;
    };
    std::vector<unsigned> dirty(static_cast<std::size_t>(nx) * ny, replacement_ ? 3u : 0u);
    for (const auto &[command, value] : pending_) {
        (void)value;
        const auto cell = command % cell_count;
        const int cx = static_cast<int>(cell % candidate.width) / kChunkSize;
        const int cy = static_cast<int>(cell / candidate.width) / kChunkSize;
        dirty[cy * nx + cx] |= 1u << (command / cell_count);
    }
    auto tile_at = [&](int layer, int cx, int cy) {
        const auto cell = static_cast<std::size_t>(cy) * candidate.width + cx;
        const auto found = pending_.find(layer * cell_count + cell);
        return found == pending_.end() ? LayerAt(candidate, layer).cells[cell] : found->second;
    };
    std::vector<Prepared> prepared;
    std::vector<Chunk> new_chunks;
    if (replacement_) new_chunks.resize(dirty.size());
    for (std::size_t index = 0; index < dirty.size(); ++index) {
        const bool collision = replacement_ || (dirty[index] & 2u) != 0 || chunks_[index].collision_dirty;
        if (dirty[index] == 0 && !collision) continue;
        Prepared work{index, dirty[index], collision, {}, {}, nullptr};
        const int start_x = static_cast<int>(index % nx) * kChunkSize;
        const int start_y = static_cast<int>(index / nx) * kChunkSize;
        const int end_x = std::min(start_x + kChunkSize, candidate.width);
        const int end_y = std::min(start_y + kChunkSize, candidate.height);
        for (int layer = 0; layer < 2; ++layer) {
            if ((work.mask & (1u << layer)) == 0) continue;
            for (int cy = start_y; cy < end_y; ++cy)
                for (int cx = start_x; cx < end_x; ++cx) {
                    const int tile = tile_at(layer, cx, cy);
                    if (tile != 0) work.tiles[layer].push_back({cx, cy, tile});
                }
        }
        if (collision)
            for (int cy = start_y; cy < end_y; ++cy) {
                int cx = start_x;
                while (cx < end_x) {
                    if (tile_at(1, cx, cy) == 0) { ++cx; continue; }
                    const int first = cx++;
                    while (cx < end_x && tile_at(1, cx, cy) != 0) ++cx;
                    work.runs.push_back({first, cy, cx - first});
                }
            }
        prepared.push_back(std::move(work));
    }
    // Create all replacement bodies before changing committed data or old
    // geometry. Disabled bodies cannot produce contacts during preparation.
    try {
        for (auto &work : prepared) {
            if (!work.collision || work.runs.empty()) continue;
            Rigidbody::EnsurePhysicsWorld();
            physics_world_ = RayCast::GetPhysicsWorld();
            b2BodyDef definition;
            definition.type = b2_staticBody;
            definition.position.Set(x, y);
            definition.enabled = false;
            work.body = physics_world_->CreateBody(&definition);
            for (const auto &run : work.runs) {
                b2PolygonShape shape;
                shape.SetAsBox(run.length * candidate.cell_size * 0.5f, candidate.cell_size * 0.5f,
                    b2Vec2((run.x + run.length * 0.5f) * candidate.cell_size,
                           (run.y + 0.5f) * candidate.cell_size), 0);
                b2FixtureDef fixture;
                fixture.shape = &shape; fixture.friction = 0; fixture.restitution = 0;
                auto *created = work.body->CreateFixture(&fixture);
                RayCast::RegisterFixture(created, actor, RayCast::FixtureKind::Collider);
            }
        }
    } catch (...) {
        for (auto &work : prepared) DestroyBody(work.body);
        throw;
    }
    bool changed = !pending_.empty();
    if (replacement_) {
        changed = data_.width != candidate.width || data_.height != candidate.height;
        if (!changed)
            for (int layer = 0; layer < 2 && !changed; ++layer)
                for (int cy = 0; cy < candidate.height && !changed; ++cy)
                    for (int cx = 0; cx < candidate.width; ++cx)
                        if (LayerAt(data_, layer).cells[static_cast<std::size_t>(cy) * data_.width + cx] != tile_at(layer, cx, cy)) {
                            changed = true;
                            break;
                        }
    }
    committing_ = true;
    const auto commands = std::move(pending_);
    pending_.clear();
    if (replacement_) {
        ClearBodies();
        data_ = std::move(*replacement_); replacement_.reset();
        chunks_ = std::move(new_chunks);
        chunks_x_ = nx; chunks_y_ = ny;
        initialized_ = true;
        // Newly prepared bodies still belong to the shared world.
        physics_world_ = RayCast::GetPhysicsWorld();
    }
    for (const auto &[command, value] : commands)
        LayerAt(data_, static_cast<int>(command / cell_count)).cells[command % cell_count] = value;
    if (changed) ++revision_;
    for (auto &work : prepared) {
        auto &chunk = chunks_[work.index];
        for (int layer = 0; layer < 2; ++layer)
            if ((work.mask & (1u << layer)) != 0) {
                chunk.tiles[layer] = std::move(work.tiles[layer]);
                ++diagnostics_.render_chunk_rebuilds;
            }
        if (work.collision) {
            DestroyBody(chunk.body);
            chunk.body = work.body;
            chunk.collision_dirty = false;
            ++diagnostics_.collision_chunk_rebuilds;
            diagnostics_.fixtures_created += work.runs.size();
        }
    }
    origin_x_ = x; origin_y_ = y;
    committed_enabled_ = next_enabled;
    for (auto &chunk : chunks_) if (chunk.body != nullptr) {
        if (chunk.body->GetPosition() != b2Vec2(x, y)) chunk.body->SetTransform(b2Vec2(x, y), 0);
        if (chunk.body->IsEnabled() != next_enabled) chunk.body->SetEnabled(next_enabled);
    }
    committing_ = false;
}
std::vector<ImageDrawRequest> Tilemap::CollectVisible(double min_x, double min_y, double max_x, double max_y) {
    FiniteFloat(min_x, "viewport minimum x"); FiniteFloat(min_y, "viewport minimum y");
    FiniteFloat(max_x, "viewport maximum x"); FiniteFloat(max_y, "viewport maximum y");
    diagnostics_.visible_tiles = 0;
    std::vector<ImageDrawRequest> draws;
    if (!HasAsset() || !HasLiveBinding() || !committed_enabled_ || max_x <= min_x || max_y <= min_y) return draws;
    const int x0 = static_cast<int>(std::clamp(std::floor((min_x - origin_x_) / data_.cell_size), 0.0, static_cast<double>(data_.width)));
    const int y0 = static_cast<int>(std::clamp(std::floor((min_y - origin_y_) / data_.cell_size), 0.0, static_cast<double>(data_.height)));
    const int x1 = static_cast<int>(std::clamp(std::ceil((max_x - origin_x_) / data_.cell_size), 0.0, static_cast<double>(data_.width)));
    const int y1 = static_cast<int>(std::clamp(std::ceil((max_y - origin_y_) / data_.cell_size), 0.0, static_cast<double>(data_.height)));
    if (x0 >= x1 || y0 >= y1) return draws;
    const std::string image = data_.resolved_image_path.empty() ? data_.atlas.image : data_.resolved_image_path.string();
    for (int layer = 0; layer < 2; ++layer)
        for (int cy = y0 / kChunkSize; cy <= (y1 - 1) / kChunkSize; ++cy)
            for (int cx = x0 / kChunkSize; cx <= (x1 - 1) / kChunkSize; ++cx)
                for (const auto &tile : chunks_[cy * chunks_x_ + cx].tiles[layer]) {
                    if (tile.x < x0 || tile.x >= x1 || tile.y < y0 || tile.y >= y1) continue;
                    ImageDrawRequest draw;
                    draw.image_name = image;
                    draw.x = origin_x_ + tile.x * data_.cell_size;
                    draw.y = origin_y_ + tile.y * data_.cell_size;
                    draw.pivot_x = 0; draw.pivot_y = 0;
                    draw.scale_x = data_.cell_size * 100.0f / data_.atlas.tile_width_px;
                    draw.scale_y = data_.cell_size * 100.0f / data_.atlas.tile_height_px;
                    draw.spritesheet_columns = data_.atlas.columns; draw.spritesheet_rows = data_.atlas.rows;
                    draw.sprite_column = (tile.id - 1) % data_.atlas.columns + 1;
                    draw.sprite_row = (tile.id - 1) / data_.atlas.columns + 1;
                    draw.sorting_order = LayerAt(data_, layer).sorting_order;
                    draws.push_back(std::move(draw));
                }
    diagnostics_.visible_tiles = draws.size();
    return draws;
}
std::size_t Tilemap::QueueVisible(double min_x, double min_y, double max_x, double max_y) {
    auto draws = CollectVisible(min_x, min_y, max_x, max_y);
    for (const auto &draw : draws) Renderer::QueueSceneImageDraw(draw);
    return draws.size();
}
b2Body *Tilemap::GetChunkBody(int x, int y) const {
    if (x < 0 || y < 0 || x >= chunks_x_ || y >= chunks_y_) return nullptr;
    return chunks_[y * chunks_x_ + x].body;
}
std::size_t Tilemap::GetFixtureCount() const {
    if (RayCast::GetPhysicsWorld() != physics_world_) return 0;
    std::size_t count = 0;
    for (const auto &chunk : chunks_) if (chunk.body != nullptr)
        for (auto *fixture = chunk.body->GetFixtureList(); fixture != nullptr; fixture = fixture->GetNext()) ++count;
    return count;
}
