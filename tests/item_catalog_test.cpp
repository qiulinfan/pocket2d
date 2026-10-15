#include "shared/inventory/ItemCatalog.h"
#include "shared/resources/ResourcePath.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
void Check(bool condition, const std::string &message) { if (!condition) throw std::runtime_error(message); }
void Reject(const std::function<void()> &action, const std::string &where) {
    try { action(); } catch (const std::exception &error) {
        Check(std::string(error.what()).size() > 0, "missing error message");
        return;
    }
    throw std::runtime_error("accepted invalid catalog: " + where);
}
void Put(const fs::path &path, const std::string &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(stream), "fixture write failed");
}
struct Fixture {
    fs::path previous = ResourcePath::ResourcesRootPath();
    fs::path root = fs::temp_directory_path() / ("pocket-items-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path project = root / "project";
    Fixture() { fs::create_directories(project / "items"); ResourcePath::SetResourcesRootPath(project); }
    ~Fixture() { ResourcePath::SetResourcesRootPath(previous); std::error_code ignored; fs::remove_all(root, ignored); }
};
const std::string valid = R"({"version":1,"items":[
 {"id":"wood","display_name":"木材","kind":"material","max_stack":99},
 {"id":"601","display_name":"Tomato","kind":"food","max_stack":9999,"health_restore":20,"description":"Food"},
 {"id":"100","display_name":"Sword","kind":"weapon","max_stack":1,"equipment_slot":"subweapon","attack":10},
 {"id":"hat","display_name":"Hat","kind":"armor","max_stack":1,"equipment_slot":"head","defense":2,"move_speed_bonus":0.5},
 {"id":"vest","display_name":"Vest","kind":"armor","max_stack":1,"equipment_slot":"chest"},
 {"id":"boots","display_name":"Boots","kind":"armor","max_stack":1,"equipment_slot":"legs","move_speed_bonus":100}
]})";
const std::string simple = R"({"id":"one","display_name":"One","kind":"material","max_stack":1})";
std::string Wrap(const std::string &item) { return "{\"version\":1,\"items\":[" + item + "]}"; }
void Run() {
    Fixture fixture;
    const auto path = fixture.project / "items/catalog.items.json";
    Put(path, valid);
    auto data = ItemCatalog::LoadResource("catalog.items.json");
    Check(data.version == 1 && data.items.size() == 6 && data.index.size() == 6, "valid schema lost entries");
    Check(ItemCatalog::Find(data, "wood")->display_name == "木材", "UTF8 display name lost");
    Check(ItemCatalog::Find(data, "wood")->description.empty() && ItemCatalog::Find(data, "wood")->attack == 0,
          "optional defaults incorrect");
    Check(ItemCatalog::Find(data, "601")->health_restore == 20 && ItemCatalog::Find(data, "hat")->move_speed_bonus == 0.5,
          "food/armor stats incorrect");
    Check(!ItemCatalog::Find(data, "WOOD") && !ItemCatalog::Find(data, "missing"), "ID lookup must be exact");
    auto renamed = data;
    renamed.items[0].id = "WOOD";
    renamed.index.emplace("injected", 100000);
    ItemCatalog::ValidateData(renamed);
    Check(!ItemCatalog::Find(renamed, "wood") && !ItemCatalog::Find(renamed, "injected") && ItemCatalog::Find(renamed, "WOOD"),
          "validation did not rebuild stale index");
    Check(ItemCatalog::Find(data, "wood"), "separate catalog changed during revalidation");
    auto two = data;
    two.items.push_back(two.items.front()); two.items.back().id = "WOOD";
    ItemCatalog::ValidateData(two);
    Check(ItemCatalog::Find(two, "WOOD") != ItemCatalog::Find(two, "wood"), "case-distinct IDs were folded");
    const auto prior_index = two.index;
    two.items.back().id = "wood";
    Reject([&] { ItemCatalog::ValidateData(two); }, "duplicate programmatic ID");
    Check(two.index == prior_index, "failed validation partially replaced derived index");

    const auto reject_data = [&](const std::function<void(ItemCatalog::Data &)> &edit, const std::string &label) {
        auto bad = data; edit(bad); Reject([&] { ItemCatalog::ValidateData(bad); }, label);
    };
    for (const auto &id : std::vector<std::string>{"", "_one", ".one", "-one", "one/two", "one\\two", "one two", "物品", std::string(65, 'a'), std::string("a\0b", 3)})
        reject_data([&](auto &bad) { bad.items[0].id = id; }, "bad ID");
    auto boundaries = data;
    boundaries.items[0].id = std::string(64, 'x');
    boundaries.items[0].display_name = std::string(256, 'x');
    boundaries.items[0].description = std::string(4096, 'x');
    boundaries.items[1].health_restore = 1000000;
    boundaries.items[2].attack = 1000000;
    boundaries.items[3].defense = 1000000;
    ItemCatalog::ValidateData(boundaries);
    for (const auto &field : {"display", "description", "kind", "slot"}) {
        reject_data([&](auto &bad) {
            auto &item = bad.items[0];
            std::string *value = std::string(field) == "display" ? &item.display_name : std::string(field) == "description" ? &item.description : std::string(field) == "kind" ? &item.kind : &item.equipment_slot;
            *value = std::string("bad\0value", 9);
        }, "NUL string");
        reject_data([&](auto &bad) {
            auto &item = bad.items[0];
            std::string *value = std::string(field) == "display" ? &item.display_name : std::string(field) == "description" ? &item.description : std::string(field) == "kind" ? &item.kind : &item.equipment_slot;
            *value = "\xc0\xaf";
        }, "invalid UTF8 string");
    }
    reject_data([](auto &bad) { bad.version = 2; }, "future version");
    reject_data([](auto &bad) { bad.items[0].display_name.clear(); }, "empty display");
    reject_data([](auto &bad) { bad.items[0].display_name.assign(257, 'x'); }, "long display");
    reject_data([](auto &bad) { bad.items[0].description.assign(4097, 'x'); }, "long description");
    reject_data([](auto &bad) { bad.items[0].kind = "seed"; }, "unknown kind");
    reject_data([](auto &bad) { bad.items[0].max_stack = 0; }, "zero stack");
    reject_data([](auto &bad) { bad.items[0].max_stack = 10000; }, "large stack");
    reject_data([](auto &bad) { bad.items[0].health_restore = 1; }, "material healing");
    reject_data([](auto &bad) { bad.items[1].health_restore = 0; }, "food zero healing");
    reject_data([](auto &bad) { bad.items[1].health_restore = 1000001; }, "food excessive healing");
    reject_data([](auto &bad) { bad.items[1].attack = 1; }, "food attack");
    reject_data([](auto &bad) { bad.items[1].equipment_slot = "head"; }, "food equipment");
    reject_data([](auto &bad) { bad.items[2].max_stack = 2; }, "stackable weapon");
    reject_data([](auto &bad) { bad.items[2].equipment_slot = "head"; }, "weapon wrong slot");
    reject_data([](auto &bad) { bad.items[2].defense = 1; }, "weapon armor stat");
    reject_data([](auto &bad) { bad.items[2].move_speed_bonus = 1; }, "weapon movement stat");
    reject_data([](auto &bad) { bad.items[2].attack = -1; }, "negative attack");
    reject_data([](auto &bad) { bad.items[3].equipment_slot = "subweapon"; }, "armor wrong slot");
    reject_data([](auto &bad) { bad.items[3].health_restore = 1; }, "armor healing");
    reject_data([](auto &bad) { bad.items[3].attack = 1; }, "armor attack");
    for (double speed : {-0.5, 100.001, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        reject_data([&](auto &bad) { bad.items[3].move_speed_bonus = speed; }, "invalid speed");

    const auto reject_json = [&](const std::string &text) {
        Put(path, text); Reject([&] { ItemCatalog::LoadFile(path); }, "malformed JSON/schema");
    };
    for (const std::string &text : std::vector<std::string>{"", "{", "[]", "null", "{}", "{\"version\":2,\"items\":[]}",
            "{\"version\":1,\"items\":{},\"extra\":1}", "{\"version\":1,\"version\":1,\"items\":[]}",
            "{\"version\":1,\"items\":[null]}", Wrap("{}"), Wrap(simple + "," + simple),
            Wrap("{\"id\":\"x\",\"display_name\":\"X\",\"kind\":\"material\",\"max_stack\":1,\"icon\":\"x.png\"}"),
            Wrap("{\"id\":\"x\",\"display_name\":\"X\",\"kind\":\"material\",\"max_stack\":1,\"id\":\"y\"}")}) reject_json(text);
    for (const auto *number : {"1.0", "1.5", "true", "\"1\"", "null", "2147483648", "9223372036854775808", "1e999"}) {
        auto text = Wrap(simple);
        const auto offset = text.find("\"max_stack\":1");
        text.replace(offset, 13, std::string("\"max_stack\":") + number);
        reject_json(text);
    }
    reject_json("{\"version\":1,\"items\":[],\"bad\":\"\xff\"}");
    reject_json(valid + std::string("\0ignored", 8));
    reject_json("{\"version\":1,\"items\":[],\"bad\":" + std::string(1000, '[') + "0" + std::string(1000, ']') + "}");
    auto maximum = data;
    maximum.items.clear();
    for (int i = 0; i < 4096; ++i) {
        auto entry = data.items[0]; entry.id = "item." + std::to_string(i); maximum.items.push_back(entry);
    }
    ItemCatalog::ValidateData(maximum);
    Check(maximum.index.size() == 4096 && ItemCatalog::Find(maximum, "item.4095"), "4096 item boundary rejected");
    maximum.items.push_back(data.items[1]);
    Reject([&] { ItemCatalog::ValidateData(maximum); }, "4097 entries");
    std::string oversized_count = "{\"version\":1,\"items\":[";
    for (int i = 0; i < 4097; ++i) { if (i) oversized_count += ','; oversized_count += simple; }
    oversized_count += "]}"; reject_json(oversized_count);
    auto exact_size = valid; exact_size.resize(4 * 1024 * 1024, ' ');
    Put(path, exact_size); Check(ItemCatalog::LoadFile(path).items.size() == 6, "exact 4MiB file rejected");
    exact_size.push_back(' '); reject_json(exact_size);

    Put(path, valid);
    Put(fixture.project / "items/nested/only.items.json", valid);
    Check(ItemCatalog::LoadResource("nested/only.items.json").items.size() == 6, "explicit nested resource failed");
    Reject([&] { ItemCatalog::LoadResource("only.items.json"); }, "basename fallback");
    for (const auto &relative : std::vector<std::string>{"", "../catalog.items.json", "/catalog.items.json", "C:/catalog.items.json", "nested\\only.items.json", "catalog.json", std::string("catalog.items.json\0suffix", 25)})
        Reject([&] { ItemCatalog::LoadResource(relative); }, "invalid resource path");
    Put(fixture.root / "outside.items.json", valid);
    std::error_code error;
    fs::create_symlink(fixture.root / "outside.items.json", fixture.project / "items/outside.items.json", error);
    if (!error) {
        Reject([&] { ItemCatalog::LoadResource("outside.items.json"); }, "outside file symlink");
        fs::create_directory_symlink(fixture.root, fixture.project / "items/escape");
        Reject([&] { ItemCatalog::LoadResource("escape/outside.items.json"); }, "outside directory symlink");
        fs::create_symlink(path, fixture.project / "items/alias.items.json");
        Check(ItemCatalog::LoadResource("alias.items.json").items.size() == 6, "inside alias rejected");
    } else {
#ifdef _WIN32
        std::cout << "symlink checks skipped: " << error.message() << '\n';
#else
        Check(false, "cannot create symlink test: " + error.message());
#endif
    }
    const auto unicode = fs::path(std::u8string(u8"目录/物品.items.json"));
    Put(fixture.project / "items" / unicode, valid);
    Check(ItemCatalog::LoadResource("目录/物品.items.json").items.size() == 6, "UTF8 resource path failed");
    Put(path, Wrap(simple));
    auto reloaded = ItemCatalog::LoadResource("catalog.items.json");
    Check(reloaded.index.size() == 1 && ItemCatalog::Find(reloaded, "one") && !ItemCatalog::Find(reloaded, "wood"), "reload kept previous index");
    Check(ItemCatalog::Find(data, "wood") && data.items.size() == 6, "reload mutated previously loaded catalog");
    Put(path, "{\"version\":1,\"items\":[]}");
    Check(ItemCatalog::LoadFile(path).index.empty(), "empty catalog did not produce empty index");
    Check(!fs::exists(fixture.project / ".pocketengine"), "catalog loader wrote editor state");
}
} // namespace
int main() {
    try { Run(); std::cout << "ItemCatalog: strict schema, stats, paths, limits and reindexing passed\n"; return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
