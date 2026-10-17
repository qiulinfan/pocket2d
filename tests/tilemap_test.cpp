#include "scene/Tilemap.h"
#include "rendering/Renderer.h"
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
    reject_json([](auto &j) { j["width"].SetInt(4097); });
    reject_json([](auto &j) { j["width"].SetInt(4096); j["height"].SetInt(1025); });
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
    Check(map.GetCells("solid", 1, 1, 2, 2) == std::vector<int>{0, 5, 3, 2}, "bulk read is row-major");
    Check(map.GetCells("solid", 0, 0, 0, 0).empty(), "an empty rectangle reads nothing");
    Reject([&] { map.GetCells("solid", 4, 4, 2, 1); }, "bulk read must stay inside the map");
    // Extra layers: named, drawn at their own order, queryable, never solid.
    auto layered = fixture.Data(4, 4);
    layered.extra_layers.push_back({"walls", {-50, std::vector<int>(16, 0)}});
    layered.extra_layers[0].layer.cells[5] = 3;
    TilemapAsset::SaveResource("layered.tilemap.json", layered);
    const auto reloaded = TilemapAsset::LoadResource("layered.tilemap.json");
    Check(reloaded.extra_layers.size() == 1 && reloaded.extra_layers[0].name == "walls" &&
          reloaded.extra_layers[0].layer.cells[5] == 3, "extra layers round-trip");
    Tilemap walls;
    walls.Initialize(reloaded, 0, 0);
    Check(walls.GetLayerNames() == std::vector<std::string>{"ground", "solid", "walls"} &&
          *walls.GetCell("walls", 1, 1) == 3, "extra layer cells read by name");
    Check(walls.SetCell("walls", 2, 2, 4), "extra layer edits queue");
    walls.CommitPending();
    Check(*walls.GetCell("walls", 2, 2) == 4 && !walls.IsSolidAt(2.5, 2.5), "extra layers commit and never collide");
    // A terrain on an extra layer re-frames that layer's neighbours, never
    // solid's: (2,1) joins the dirt at (1,1) and (3,1), which also connects to
    // the outside on its right (mask 10 for both).
    auto walls_terrain = reloaded;
    walls_terrain.autotile = data.autotile;
    Tilemap framed;
    framed.Initialize(walls_terrain, 0, 0);
    Check(framed.SetTerrain("walls", 2, 1, "dirt") && framed.SetTerrain("walls", 3, 1, "dirt"), "extra layer terrain queues");
    framed.CommitPending();
    Check(*framed.GetCell("walls", 2, 1) == 3 && *framed.GetCell("walls", 3, 1) == 3 &&
          *framed.GetCell("solid", 2, 1) == walls_terrain.solid.cells[6], "extra layer neighbours re-frame on their own layer");
    framed.OnDestroy();
    bool drawn = false;
    for (const auto &draw : walls.CollectVisible(0, 0, 4, 4)) drawn |= draw.sorting_order == -50;
    Check(drawn, "extra layers draw at their own sorting order");
    auto clash = layered; clash.extra_layers.push_back({"walls", {0, std::vector<int>(16, 0)}});
    Reject([&] { TilemapAsset::ValidateData(clash); }, "duplicate layer names reject");
    auto bad = layered; bad.extra_layers[0].name = "Walls!";
    Reject([&] { TilemapAsset::ValidateData(bad); }, "layer names are [a-z0-9_]");
    walls.OnDestroy();
    // A map picture: one pixel per cell (or per step cells), by terrain colour.
    Tilemap::MapOptions picture;
    picture.image = "dynamic:map-test";
    picture.colors = {{"dirt", {100, 60, 30}}};
    picture.sky = {0, 0, 255, 255};
    picture.cave = {0, 0, 0, 255};
    picture.cave_y = 3;
    map.ComputeMap(0, 0, 5, 5, picture);
    int map_width = 0, map_height = 0;
    Check(Renderer::GetImageSize("dynamic:map-test", map_width, map_height) && map_width == 5 && map_height == 5,
          "the map picture has a pixel per cell");
    picture.step = 2;
    map.ComputeMap(0, 0, 5, 5, picture);
    Check(Renderer::GetImageSize("dynamic:map-test", map_width, map_height) && map_width == 3 && map_height == 3,
          "a step of two halves the picture, rounding up");
    picture.step = 0;
    Reject([&] { map.ComputeMap(0, 0, 5, 5, picture); }, "a map step below one rejects");
    // Fog of war: revealed cells round-trip through their run-length text.
    Check(!map.IsRevealed(2, 2) && map.SaveRevealed() == "25", "nothing is revealed at first");
    map.RevealMap(0, 0, 1);
    Check(map.IsRevealed(0, 0) && map.IsRevealed(1, 0) && map.IsRevealed(0, 1) && !map.IsRevealed(1, 1),
          "reveal marks a disc of cells");
    const std::string runs = map.SaveRevealed();
    Check(runs == "0,2,3,1,19", "revealed cells save as alternating runs");
    map.LoadRevealed("25");
    Check(!map.IsRevealed(0, 0), "loading runs replaces the revealed cells");
    map.LoadRevealed(runs);
    Check(map.IsRevealed(1, 0) && !map.IsRevealed(4, 4), "saved runs load back");
    Reject([&] { map.LoadRevealed("3,4"); }, "runs must cover the map exactly");
    picture.step = 1;
    picture.fog = std::array<std::uint8_t, 4>{0, 0, 0, 255};
    map.ComputeMap(0, 0, 5, 5, picture);
    map.OnDestroy();
}
void Lighting(const Fixture &fixture) {
    // Open sky over three columns of solid cells four deep.
    auto data = fixture.Data(3, 5);
    data.ground.cells.assign(15, 0);
    for (std::size_t cell = 3; cell < 15; ++cell) data.solid.cells[cell] = 1;
    TilemapAsset::SaveResource("light.tilemap.json", data);
    Tilemap map;
    map.Initialize(TilemapAsset::LoadResource("light.tilemap.json"), 0, 0);
    Tilemap::LightOptions light;
    light.image = "dynamic:light-test";
    light.air_decay = 0.9f; light.solid_decay = 0.5f;
    const auto levels = map.ComputeLight(0, -1, 3, 6, light);
    const auto at = [&](const std::vector<float> &rgb, int x, int y, int channel) {
        return rgb[(static_cast<std::size_t>(y + 1) * 3 + x) * 3 + channel];
    };
    Check(at(levels, 1, -1, 0) == 1.0f && at(levels, 1, 0, 2) == 1.0f, "sky reaches open cells and above the map");
    Check(std::abs(at(levels, 1, 1, 0) - 0.9f) < 1e-5f, "the top solid cell keeps the light leaving the air above it");
    Check(std::abs(at(levels, 1, 2, 1) - 0.45f) < 1e-5f && std::abs(at(levels, 1, 3, 1) - 0.225f) < 1e-5f,
          "light halves through each solid cell: a gradual fade, not a cutoff");
    int width = 0, height = 0;
    Check(Renderer::GetImageSize("dynamic:light-test", width, height) && width == 3 && height == 6,
          "the light map is a dynamic image, one pixel per cell");
    light.sky = 0.0f;
    light.sources = {1.5f, 3.5f, 1.0f, 0.5f, 0.2f};
    const auto lit = map.ComputeLight(0, -1, 3, 6, light);
    Check(at(lit, 1, 3, 0) == 1.0f && at(lit, 1, 3, 1) == 0.5f && std::abs(at(lit, 1, 3, 2) - 0.2f) < 1e-6f,
          "a coloured source lights its cell in its own colour");
    Check(std::abs(at(lit, 1, 2, 0) - 0.5f) < 1e-5f && std::abs(at(lit, 1, 2, 1) - 0.25f) < 1e-5f &&
              std::abs(at(lit, 1, 2, 2) - 0.1f) < 1e-5f,
          "coloured light keeps its hue as it decays");
    auto bad = light;
    bad.solid_decay = 1.5f;
    Reject([&] { map.ComputeLight(0, 0, 2, 2, bad); }, "decay above 1 rejects");
    bad = light;
    bad.sources = {1.0f, 1.0f, 1.0f};
    Reject([&] { map.ComputeLight(0, 0, 2, 2, bad); }, "sources come in x, y, r, g, b groups");
    bad = light;
    bad.image = "light-without-prefix";
    Reject([&] { map.ComputeLight(0, 0, 2, 2, bad); }, "dynamic images need the prefix");
    map.OnDestroy();
}
} // namespace
int main() {
    try {
        Fixture fixture;
        Schema(fixture); CoordinatesAndChunks(fixture); CollisionAndLifetime(fixture);
        RealPassageAndCallback(fixture); EnabledCommitSnapshot(fixture); Autotile(fixture); Lighting(fixture);
        std::cout << "PASS tilemap schema, isolated state, coordinates, dirty chunks, culling, real collision/callback lifecycle, autotile\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL tilemap: " << error.what() << '\n'; return 1;
    }
}
