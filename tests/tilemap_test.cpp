#include "scene/Tilemap.h"
#include "physics/RayCast.h"
#include "physics/Rigidbody.h"
#include "shared/resources/ResourcePath.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "SDL3/SDL.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
void Reject(const std::function<void()> &operation, const char *message) {
    bool rejected = false;
    try { operation(); } catch (const std::exception &) { rejected = true; }
    Check(rejected, message);
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("pocket2d-tilemap-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(root / "images");
        std::filesystem::create_directories(root / "maps");
        ResourcePath::SetResourcesRootPath(root);
        auto *image = SDL_CreateSurface(8, 4, SDL_PIXELFORMAT_RGBA8888);
        Check(image != nullptr, "create atlas");
        SDL_FillSurfaceRect(image, nullptr, SDL_MapSurfaceRGBA(image, 255, 255, 255, 255));
        Check(SDL_SaveBMP(image, (root / "images/atlas.bmp").string().c_str()), "save atlas");
        SDL_DestroySurface(image);
    }
    ~Fixture() {
        Rigidbody::DestroyPhysicsWorld();
        std::error_code error; std::filesystem::remove_all(root, error);
    }
    TilemapAsset::Data Data(int width = 64, int height = 64) const {
        TilemapAsset::Data data;
        data.width = width; data.height = height;
        data.atlas = {"atlas.bmp", 4, 2, 2, 2};
        data.ground.cells.assign(static_cast<std::size_t>(width) * height, 1);
        data.solid.cells.assign(static_cast<std::size_t>(width) * height, 0);
        TilemapAsset::ValidateAtlasImage(data, root / "images");
        return data;
    }
};
void Schema(const Fixture &fixture) {
    auto data = fixture.Data(4, 4);
    data.ground.cells.back() = 8;
    TilemapAsset::SaveResource("schema.tilemap.json", data);
    const auto read = TilemapAsset::LoadResource("schema.tilemap.json");
    Check(read.ground.cells == data.ground.cells && read.solid.cells == data.solid.cells,
          "map save/load must preserve both independent layers");
    Check(read.resolved_image_path.is_absolute(), "atlas must resolve once at load");
    const auto file = fixture.root / "maps/schema.tilemap.json";
    std::ifstream input(file);
    const std::string valid((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto reject_json = [&](const std::function<void(rapidjson::Document &)> &change) {
        rapidjson::Document json; json.Parse(valid.c_str()); change(json);
        rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer); json.Accept(writer);
        const auto invalid = fixture.root / "maps/invalid.tilemap.json";
        std::ofstream(invalid) << buffer.GetString();
        Reject([&] { TilemapAsset::LoadFile(invalid, fixture.root / "images"); }, "invalid typed schema must reject");
    };
    reject_json([](auto &j) { j["version"].SetInt(2); });
    reject_json([](auto &j) { j.AddMember("width", 4, j.GetAllocator()); });
    reject_json([](auto &j) { j["width"].SetInt(513); });
    reject_json([](auto &j) { j["height"].SetInt(0); });
    reject_json([](auto &j) { j["width"].SetDouble(4.5); });
    reject_json([](auto &j) { j["cell_size"].SetDouble(0); });
    reject_json([](auto &j) { j["atlas"]["columns"].SetInt(0); });
    reject_json([](auto &j) { j["atlas"]["tile_width_px"].SetInt(3); });
    reject_json([](auto &j) { j["layers"]["ground"]["cells"].PopBack(); });
    reject_json([](auto &j) { j["layers"]["ground"]["cells"][0].SetInt(9); });
    reject_json([](auto &j) { j["layers"]["ground"]["cells"][0].SetInt(-1); });
    reject_json([](auto &j) { j["layers"]["solid"]["cells"][0].SetString("5"); });
    reject_json([](auto &j) { j["atlas"].RemoveMember("image"); });
    reject_json([](auto &j) { j["layers"].AddMember("other", rapidjson::Value(rapidjson::kObjectType), j.GetAllocator()); });
    Reject([&] { TilemapAsset::LoadResource("../outside.tilemap.json"); }, "map traversal must reject");
    auto invalid = data; invalid.atlas.image = "../atlas.bmp";
    Reject([&] { TilemapAsset::ValidateData(invalid); }, "atlas traversal must reject");
    invalid = data; invalid.cell_size = std::numeric_limits<float>::infinity();
    Reject([&] { TilemapAsset::ValidateData(invalid); }, "nonfinite cell size must reject");
    invalid = data; invalid.ground.cells[0] = 99;
    Reject([&] { TilemapAsset::SaveFile(file, invalid); }, "invalid save must reject before touching disk");
    Check(TilemapAsset::LoadResource("schema.tilemap.json").ground.cells[0] == 1,
          "rejected save must retain original resource");
}
void CoordinatesAndChunks(const Fixture &fixture) {
    Tilemap empty;
    empty.OnStart(); empty.CommitPending();
    Check(!empty.HasAsset() && !empty.GetCell("ground", 0, 0) &&
          empty.CollectVisible(0, 0, 1, 1).empty(), "unconfigured map must remain inert");
    auto data = fixture.Data();
    auto overflow = data; overflow.cell_size = 1e38f;
    Reject([&] { Tilemap::ValidateInitialization(overflow, 0, 0); },
           "preflight must reject unrepresentable world extents without instantiating a map");
    Tilemap map, independent;
    map.Initialize(data, -8, -8); independent.Initialize(data, -8, -8);
    Check(map.GetRevision() == 0 && *map.GetCell("ground", 8, 8) == 1, "frame-zero data must be queryable");
    Check(map.WorldToCell(-8, -8) == Vector2(0, 0), "origin converts exactly");
    Check(map.WorldToCell(-8.001, -8).x == -1, "negative coordinates use floor");
    Check(map.WorldToCell(-7, -8).x == 1, "cell edge uses exact next cell");
    Check(map.WorldToCell(56, -8).x == 64, "right boundary stays out of range");
    Check(map.GetCellCenter(8, 8) == Vector2(0.5f, 0.5f), "cell center conversion");
    Check(map.GetCellCenter(-1, -1) == Vector2(-8.5f, -8.5f), "out-of-map centers remain mathematical");
    Check(!map.GetCell("solid", -1, 0) && *map.GetCell("solid", 0, 0) == 0, "OOB differs from empty");
    Reject([&] { map.InBounds(1.2, 1); }, "fractional coordinate must reject");
    Reject([&] { map.WorldToCell(std::numeric_limits<double>::infinity(), 0); }, "nonfinite world coordinate rejects");
    Reject([&] { map.SetCell("unknown", 0, 0, 1); }, "unknown layer rejects");
    Reject([&] { map.SetCell("ground", 0, 0, 9); }, "atlas overflow rejects");
    Reject([&] { map.SetCell("ground", 0, 0, 1.2); }, "fractional ID rejects");
    map.CommitPending(); map.ResetDiagnostics();
    Check(map.SetCell("ground", 8, 8, 2), "new edit must queue");
    Check(!map.SetCell("ground", 8, 8, 2), "duplicate pending write must be ignored");
    Check(*map.GetCell("ground", 8, 8) == 1, "queries must not flush pending edits");
    map.CommitPending();
    Check(map.GetRevision() == 1 && *map.GetCell("ground", 8, 8) == 2, "commit advances revision once");
    Check(*independent.GetCell("ground", 8, 8) == 1 && data.ground.cells[8 * 64 + 8] == 1,
          "instances and source data must stay independent");
    Check(map.GetDiagnostics().render_chunk_rebuilds == 1 && map.GetDiagnostics().collision_chunk_rebuilds == 0,
          "ground edit rebuilds one render chunk and no collision");
    map.ResetDiagnostics();
    map.SetCell("ground", 8, 8, 3); map.SetCell("ground", 8, 8, 2); map.CommitPending();
    Check(map.GetRevision() == 1 && map.GetDiagnostics().render_chunk_rebuilds == 0,
          "write restored to original must not rebuild or increment revision");
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) map.SetCell("ground", x, y, 3);
    map.CommitPending();
    Check(map.GetDiagnostics().render_chunk_rebuilds == 1, "64 writes in one chunk rebuild once");
    map.ResetDiagnostics();
    map.SetCell("ground", 31, 2, 4); map.SetCell("ground", 32, 2, 4); map.CommitPending();
    Check(map.GetDiagnostics().render_chunk_rebuilds == 2, "31/32 boundary rebuilds exactly two chunks");
    map.ResetDiagnostics();
    for (int i = 0; i < 20; ++i) map.CommitPending();
    Check(map.GetDiagnostics().render_chunk_rebuilds == 0 && map.GetDiagnostics().collision_chunk_rebuilds == 0,
          "stable ticks do not rebuild chunks");
    Check(map.CollectVisible(-8, -8, -7, -7).size() == 1, "exact viewport edge draws only one cell");
    Check(map.CollectVisible(-100, -100, -99, -99).empty(), "outside viewport draws nothing");
    Check(map.CollectVisible(23, -8, 25, -7).size() == 2, "culling across chunk boundary");
    map.SetCell("ground", 0, 0, 8); map.CommitPending();
    const auto draw = map.CollectVisible(-8, -8, -7, -7).front();
    Check(draw.sprite_column == 4 && draw.sprite_row == 2 && draw.pivot_x == 0 && draw.pivot_y == 0,
          "last atlas ID and top-left pivot must map correctly");
    Check(draw.scale_x == 50 && draw.scale_y == 50, "native atlas pixels must map to cell world size");
    const auto revision = map.GetRevision();
    auto replaced = map.GetAsset(); replaced.ground.cells[0] = 1;
    map.ReplaceAsset(replaced); map.SetCell("ground", 0, 0, 8);
    Check(*map.GetCell("ground", 0, 0) == 8, "asset replacement stays pending");
    map.CommitPending();
    Check(map.GetRevision() == revision, "replacement edited back to committed data does not advance revision");
    Reject([&] { map.CommitPending(0, 0, 10); }, "rotated map must reject");
}
void CollisionAndLifetime(const Fixture &fixture) {
    auto data = fixture.Data();
    for (int x = 0; x < 64; ++x) data.solid.cells[x] = 5;
    Actor owner; owner.uid = 10;
    Tilemap map; map.actor = &owner; map.Initialize(data); map.CommitPending();
    Check(map.GetFixtureCount() == 2, "one solid row is one collision outline per chunk");
    const b2BodyId left_body = map.GetChunkBody(0, 0);
    const b2BodyId right_body = map.GetChunkBody(1, 0);
    Check(B2_IS_NON_NULL(left_body) && B2_IS_NON_NULL(right_body), "solid chunks own static bodies");
    map.ResetDiagnostics(); map.SetCell("solid", 31, 0, 0); map.CommitPending();
    Check(!(map.GetChunkBody(0, 0) == left_body) && map.GetChunkBody(1, 0) == right_body,
          "solid edit replaces only affected chunk body");
    Check(map.GetDiagnostics().collision_chunk_rebuilds == 1, "solid chunk rebuild count");
    Check(!RayCast::Cast({31.5f, -1}, {0, 1}, 3), "removed solid cell leaves raycast hole");
    Check(RayCast::Cast({32.5f, -1}, {0, 1}, 3).has_value(), "neighbor chunk still collides");
    Tilemap other; Actor other_owner; other_owner.uid = 11;
    other.actor = &other_owner; other.Initialize(data, 100, 0); other.CommitPending();
    map.enabled = false;
    Check(b2Body_IsEnabled(map.GetChunkBody(1, 0)) && !map.CollectVisible(0, 0, 64, 1).empty(),
          "desired disable must not hide rendering before collision commit");
    map.CommitPending();
    Check(!RayCast::Cast({32.5f, -1}, {0, 1}, 3), "disabled map removes collision proxies");
    Check(map.GetCell("solid", 32, 0) == 5 && map.CollectVisible(0, 0, 64, 1).empty(),
          "disabled map remains queryable without rendering");
    map.SetCell("solid", 32, 0, 0); map.CommitPending();
    map.enabled = true;
    Check(!b2Body_IsEnabled(map.GetChunkBody(1, 0)) && map.CollectVisible(0, 0, 64, 1).empty(),
          "desired enable must not reveal rendering before collision commit");
    map.CommitPending();
    Check(b2Body_IsEnabled(map.GetChunkBody(1, 0)) && !map.CollectVisible(0, 0, 64, 1).empty(),
          "enable commit changes rendering and collision together");
    Check(!RayCast::Cast({32.5f, -1}, {0, 1}, 3) && RayCast::Cast({33.5f, -1}, {0, 1}, 3),
          "reenabling uses current committed cells");
    map.ResetDiagnostics(); const b2BodyId unchanged = map.GetChunkBody(1, 0);
    map.CommitPending(0, 10);
    Check(map.GetChunkBody(1, 0) == unchanged && map.GetDiagnostics().collision_chunk_rebuilds == 0,
          "translation updates body transform without geometry rebuild");
    Check(map.GetCellCenter(0, 0) == Vector2(0.5f, 10.5f), "translation and cell queries agree");
    map.OnDestroy();
    map.enabled = true; map.CommitPending();
    Check(map.CollectVisible(0, 0, 64, 1).empty() && map.GetFixtureCount() == 0,
          "desired enabled on destroyed map cannot revive rendering or bodies");
    Check(!map.SetCell("solid", 0, 0, 1), "destroyed component must not accept changes");
    Check(RayCast::Cast({132.5f, -1}, {0, 1}, 3).has_value(), "destroying one map preserves another");
    other.OnDestroy();
}
b2BodyId CreatePlayer(b2WorldId world, float x, float y) {
    b2BodyDef definition = b2DefaultBodyDef();
    definition.type = b2_dynamicBody; definition.position = {x, y};
    definition.gravityScale = 0; definition.fixedRotation = true;
    const b2BodyId player = b2CreateBody(world, &definition);
    b2ShapeDef shape = b2DefaultShapeDef();
    shape.enableContactEvents = true;
    const b2Circle circle{{0, 0}, 0.25f};
    b2CreateCircleShape(player, &shape, &circle);
    return player;
}
void Step(b2WorldId world) { b2World_Step(world, 1.0f / 60, 4); }
void RealPassageAndCallback(const Fixture &fixture) {
    Rigidbody::DestroyPhysicsWorld();
    auto data = fixture.Data(8, 4);
    for (int y = 0; y < 4; ++y) data.solid.cells[y * 8 + 4] = 5;
    Actor owner; owner.uid = 10;
    Tilemap map; map.actor = &owner; map.Initialize(data); map.CommitPending();
    const b2WorldId world = RayCast::GetPhysicsWorld();
    b2BodyId player = CreatePlayer(world, 3.5f, 1.5f);
    for (int i = 0; i < 30; ++i) { b2Body_SetLinearVelocity(player, {6, 0}); Step(world); }
    Check(b2Body_GetPosition(player).x <= 3.80f, "real Box2D wall must stop player");
    map.SetCell("solid", 4, 1, 0); map.CommitPending();
    for (int i = 0; i < 30; ++i) { b2Body_SetLinearVelocity(player, {6, 0}); Step(world); }
    Check(b2Body_GetPosition(player).x > 4.5f, "real player must pass opened cell");
    b2DestroyBody(player);
    map.SetCell("solid", 4, 1, 5); map.CommitPending();
    // Contact events are handled after the step; an edit made while handling
    // them stays pending until the next commit.
    player = CreatePlayer(world, 3.5f, 1.5f);
    bool called = false; int during = 0;
    for (int i = 0; i < 30 && !called; ++i) {
        b2Body_SetLinearVelocity(player, {6, 0}); Step(world);
        if (b2World_GetContactEvents(world).beginCount == 0) continue;
        called = true; map.SetCell("solid", 4, 1, 0);
        during = *map.GetCell("solid", 4, 1);
    }
    Check(called && during == 5 && map.GetCell("solid", 4, 1) == 5,
          "contact handler writes must remain pending for the completed physics step");
    map.CommitPending();
    Check(map.GetCell("solid", 4, 1) == 0, "next safe commit applies callback edit");
    b2DestroyBody(player); map.OnDestroy();
}
void EnabledCommitSnapshot(const Fixture &fixture) {
    Rigidbody::DestroyPhysicsWorld();
    auto data = fixture.Data(64, 1);
    data.solid.cells[1] = 5; data.solid.cells[33] = 5;
    Tilemap map; map.Initialize(data); map.CommitPending();
    const b2WorldId world = RayCast::GetPhysicsWorld();
    std::vector<b2BodyId> players;
    for (float x : {1.5f, 33.5f}) players.push_back(CreatePlayer(world, x, -0.1f));
    Step(world);
    map.enabled = false; map.CommitPending();
    Check(!b2Body_IsEnabled(map.GetChunkBody(0, 0)) && !b2Body_IsEnabled(map.GetChunkBody(1, 0)) &&
          map.CollectVisible(0, 0, 64, 1).empty(),
          "one enabled snapshot must apply to all bodies and rendering");
    // Disabling ended both touches; Box2D reports that after the next step.
    Step(world);
    Check(b2World_GetContactEvents(world).endCount == 2, "disabling the map must end both contacts");
    map.enabled = true;
    Check(!b2Body_IsEnabled(map.GetChunkBody(0, 0)) && map.CollectVisible(0, 0, 64, 1).empty(),
          "an enabled write from an end-contact handler waits for the next commit");
    map.CommitPending();
    Check(b2Body_IsEnabled(map.GetChunkBody(0, 0)) && b2Body_IsEnabled(map.GetChunkBody(1, 0)) &&
          !map.CollectVisible(0, 0, 64, 1).empty(), "callback enabled write applies on next commit");
    for (const b2BodyId player : players) b2DestroyBody(player);
    map.OnDestroy();
}
// Terrain "dirt" (any neighbour connects) uses IDs 1-4 and "ore" (same
// terrain only) 5-8; mask m maps to ID (m % 4) + base, so the up (1) and right
// (2) bits are visible in the chosen ID.
void Autotile(const Fixture &fixture) {
    auto data = fixture.Data(5, 5);
    data.ground.cells.assign(25, 0);
    TilemapAsset::Terrain dirt{"dirt", false, {}}, ore{"ore", true, {}};
    for (int mask = 0; mask < 16; ++mask) {
        dirt.frames[mask] = {mask % 4 + 1};
        ore.frames[mask] = {mask % 4 + 5};
    }
    data.autotile = {true, {dirt, ore}};
    const auto at = [](int x, int y) { return static_cast<std::size_t>(y) * 5 + x; };
    data.solid.cells[at(1, 2)] = 1; data.solid.cells[at(2, 2)] = 1;   // dirt pair
    data.solid.cells[at(2, 1)] = 5;                                  // ore above the right one
    data.solid.cells[at(0, 0)] = 1;                                  // corner dirt
    TilemapAsset::SaveResource("autotile.tilemap.json", data);
    const auto loaded = TilemapAsset::LoadResource("autotile.tilemap.json");
    Check(loaded.autotile.terrains.size() == 2 && loaded.autotile.frame_on_load &&
          loaded.autotile.terrains[1].connect_same, "autotile rules must round-trip");
    Check(loaded.solid.cells[at(1, 2)] == 3, "left of a pair connects right only (mask 2)");
    Check(loaded.solid.cells[at(2, 2)] == 2, "any-connect dirt joins the ore above (mask 9)");
    Check(loaded.solid.cells[at(2, 1)] == 5, "same-connect ore ignores dirt (mask 0)");
    Check(loaded.solid.cells[at(0, 0)] == 2, "outside the map connects (mask 9)");
    auto duplicate = data;
    duplicate.autotile.terrains[1].frames[3] = {1};
    Reject([&] { TilemapAsset::ValidateData(duplicate); }, "a tile ID must belong to one terrain");
    auto missing = data;
    missing.autotile.terrains[0].frames[7].clear();
    Reject([&] { TilemapAsset::ValidateData(missing); }, "every mask needs a tile");

    Tilemap map;
    map.Initialize(loaded, 0, 0);
    Check(*map.GetTerrain("solid", 2, 1) == "ore" && map.GetTerrain("solid", 4, 4)->empty() &&
          !map.GetTerrain("solid", 9, 9), "terrain names, empty cells and out-of-map differ");
    Check(map.SetTerrain("solid", 3, 2, "dirt"), "placing a terrain queues an edit");
    Check(*map.GetCell("solid", 2, 2) == 2, "terrain edits stay pending until commit");
    map.CommitPending();
    Check(*map.GetCell("solid", 3, 2) == 1, "new cell framed from its left neighbour (mask 8)");
    Check(*map.GetCell("solid", 2, 2) == 4, "neighbour re-framed: up, right and left (mask 11)");
    Check(map.SetTerrain("solid", 3, 2, ""), "clearing a terrain queues an edit");
    map.CommitPending();
    Check(*map.GetCell("solid", 3, 2) == 0 && *map.GetCell("solid", 2, 2) == 2,
          "clearing re-frames the neighbour back");
    Reject([&] { map.SetTerrain("solid", 1, 1, "lava"); }, "unknown terrain rejects");
    map.OnDestroy();
}
} // namespace
int main() {
    try {
        Fixture fixture;
        Schema(fixture); CoordinatesAndChunks(fixture); CollisionAndLifetime(fixture);
        RealPassageAndCallback(fixture); EnabledCommitSnapshot(fixture); Autotile(fixture);
        std::cout << "PASS tilemap schema, isolated state, coordinates, dirty chunks, culling, real collision/callback lifecycle, autotile\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL tilemap: " << error.what() << '\n'; return 1;
    }
}
