#include "shared/inventory/ItemCatalog.h"
#include "shared/resources/ResourcePath.h"
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/memorystream.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace {
constexpr std::size_t kMaxBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxItems = 4096;
using Json = rapidjson::Value;

void Require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error("item catalog: " + message);
}
bool UTF8(const std::string &value) {
    rapidjson::MemoryStream stream(value.data(), value.size());
    while (stream.Tell() < value.size()) {
        unsigned codepoint = 0;
        if (!rapidjson::UTF8<>::Decode(stream, &codepoint)) return false;
    }
    return true;
}
void Text(const std::string &value, std::size_t maximum, const std::string &where, bool nonempty = false) {
    Require(!nonempty || !value.empty(), where + " must not be empty");
    Require(value.size() <= maximum, where + " exceeds its UTF-8 byte limit");
    Require(value.find('\0') == std::string::npos && UTF8(value), where + " must be UTF-8 without NUL");
}
bool Identifier(const std::string &value) {
    if (value.empty() || value.size() > 64) return false;
    const auto alnum = [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(value.front())) return false;
    for (unsigned char c : value) if (!alnum(c) && c != '_' && c != '.' && c != '-') return false;
    return true;
}
std::filesystem::path UTF8Path(const std::string &text) { return std::filesystem::path(std::u8string(text.begin(), text.end())); }
std::string PathText(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}
void Extension(const std::filesystem::path &path) {
    Require(PathText(path.filename()).ends_with(".items.json"), "resource must end in .items.json");
}
std::filesystem::path Resolve(const std::string &relative) {
    Text(relative, kMaxBytes, "resource path", true);
    Require(relative.find('\\') == std::string::npos && (relative.size() < 2 || relative[1] != ':'),
            "resource path must use portable relative '/' separators");
    const auto path = UTF8Path(relative);
    Require(!path.is_absolute() && !path.has_root_name() && !path.has_root_directory(), "resource path must be relative");
    for (const auto &part : path) Require(part != "..", "resource path must not contain '..'");
    const auto root = std::filesystem::weakly_canonical(std::filesystem::absolute(ResourcePath::ResourceSubdirectory("items")));
    const auto resolved = std::filesystem::weakly_canonical(root / path);
    Require(ResourcePath::IsPathWithinDirectory(resolved, root), "resource resolves outside items directory");
    return resolved;
}
std::string ReadBounded(const std::filesystem::path &path) {
    Require(std::filesystem::is_regular_file(path), "resource is not a regular file: " + PathText(path));
    const auto size = std::filesystem::file_size(path);
    Require(size <= kMaxBytes, "resource exceeds 4 MiB");
    std::ifstream stream(path, std::ios::binary);
    Require(stream.is_open(), "cannot open resource: " + PathText(path));
    std::string text;
    text.reserve(static_cast<std::size_t>(size));
    std::array<char, 8192> block;
    while (stream) {
        stream.read(block.data(), block.size());
        const auto count = static_cast<std::size_t>(stream.gcount());
        Require(text.size() + count <= kMaxBytes, "resource exceeds 4 MiB");
        text.append(block.data(), count);
    }
    Require(stream.eof(), "failed to read resource");
    Require(text.find('\0') == std::string::npos, "JSON contains a raw NUL byte");
    // The actual schema has three nested containers. Bound malformed input
    // before DOM allocation, while respecting braces inside JSON strings.
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (const char c : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') Require(++depth <= 8, "JSON nesting exceeds catalog limits");
        else if ((c == '}' || c == ']') && depth > 0) --depth;
    }
    return text;
}
void Keys(const Json &object, std::initializer_list<std::string_view> required,
          std::initializer_list<std::string_view> optional, const std::string &where) {
    Require(object.IsObject(), where + " must be an object");
    for (auto key : required) Require(object.HasMember(key.data()), where + "." + std::string(key) + " is required");
    for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member) {
        const std::string_view name(member->name.GetString(), member->name.GetStringLength());
        Require(std::find(required.begin(), required.end(), name) != required.end() ||
                std::find(optional.begin(), optional.end(), name) != optional.end(), where + ": unknown field " + std::string(name));
        for (auto previous = object.MemberBegin(); previous != member; ++previous)
            Require(std::string_view(previous->name.GetString(), previous->name.GetStringLength()) != name,
                    where + ": duplicate field " + std::string(name));
    }
}
std::string String(const Json &object, const char *key, const std::string &where) {
    Require(object[key].IsString(), where + "." + key + " must be a string");
    return {object[key].GetString(), object[key].GetStringLength()};
}
int Integer(const Json &object, const char *key, const std::string &where) {
    Require(object[key].IsInt(), where + "." + key + " must be an exact signed 32-bit JSON integer");
    return object[key].GetInt();
}
} // namespace

namespace ItemCatalog {
void ValidateData(Data &data) {
    Require(data.version == 1, "unsupported version");
    Require(data.items.size() <= kMaxItems, "catalog exceeds 4096 items");
    decltype(data.index) index;
    index.reserve(data.items.size());
    for (std::size_t i = 0; i < data.items.size(); ++i) {
        const auto &item = data.items[i];
        const auto where = "items[" + std::to_string(i) + "]";
        Require(Identifier(item.id), where + ".id must match [A-Za-z0-9][A-Za-z0-9_.-]{0,63}");
        Require(index.emplace(item.id, i).second, where + ": duplicate item ID '" + item.id + "'");
        Text(item.display_name, 256, where + ".display_name", true);
        Text(item.description, 4096, where + ".description");
        Text(item.kind, 16, where + ".kind", true);
        Text(item.equipment_slot, 16, where + ".equipment_slot");
        Require(item.kind == "material" || item.kind == "food" || item.kind == "weapon" || item.kind == "armor", where + ": unsupported kind");
        Require(item.equipment_slot.empty() || item.equipment_slot == "subweapon" || item.equipment_slot == "head" ||
                item.equipment_slot == "chest" || item.equipment_slot == "legs", where + ": unsupported equipment slot");
        Require(item.max_stack >= 1 && item.max_stack <= 9999, where + ".max_stack must be 1..9999");
        Require(item.health_restore >= 0 && item.health_restore <= 1000000 && item.attack >= 0 && item.attack <= 1000000 &&
                item.defense >= 0 && item.defense <= 1000000, where + ": integer stats must be 0..1000000");
        Require(std::isfinite(item.move_speed_bonus) && item.move_speed_bonus >= 0 && item.move_speed_bonus <= 100,
                where + ".move_speed_bonus must be finite and 0..100");
        if (item.kind == "material" || item.kind == "food") {
            Require(item.equipment_slot.empty() && item.attack == 0 && item.defense == 0 && item.move_speed_bonus == 0,
                    where + ": food/material cannot have equipment or combat stats");
            Require(item.kind == "food" ? item.health_restore >= 1 : item.health_restore == 0,
                    where + ": food requires health_restore >=1; material requires zero");
        } else {
            Require(item.max_stack == 1 && item.health_restore == 0, where + ": equipment requires max_stack 1 and no healing");
            if (item.kind == "weapon")
                Require(item.equipment_slot == "subweapon" && item.defense == 0 && item.move_speed_bonus == 0,
                        where + ": weapon requires subweapon slot and no armor stats");
            else
                Require((item.equipment_slot == "head" || item.equipment_slot == "chest" || item.equipment_slot == "legs") && item.attack == 0,
                        where + ": armor requires head/chest/legs slot and no attack");
        }
    }
    data.index = std::move(index);
}
Data LoadFile(const std::filesystem::path &path) {
    Text(PathText(path), kMaxBytes, "file path", true);
    Extension(path);
    const auto text = ReadBounded(path);
    rapidjson::Document json;
    json.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(text.data(), text.size());
    Require(!json.HasParseError(), "invalid JSON at byte " + std::to_string(json.GetErrorOffset()) + ": " + rapidjson::GetParseError_En(json.GetParseError()));
    Keys(json, {"version", "items"}, {}, "catalog");
    Data data;
    data.version = Integer(json, "version", "catalog");
    Require(json["items"].IsArray() && json["items"].Size() <= kMaxItems, "items must be an array of at most 4096 entries");
    data.items.reserve(json["items"].Size());
    for (const auto &entry : json["items"].GetArray()) {
        const auto where = "items[" + std::to_string(data.items.size()) + "]";
        Keys(entry, {"id", "display_name", "kind", "max_stack"},
             {"description", "equipment_slot", "health_restore", "attack", "defense", "move_speed_bonus"}, where);
        Definition item;
        item.id = String(entry, "id", where);
        item.display_name = String(entry, "display_name", where);
        item.kind = String(entry, "kind", where);
        item.max_stack = Integer(entry, "max_stack", where);
        if (entry.HasMember("description")) item.description = String(entry, "description", where);
        if (entry.HasMember("equipment_slot")) item.equipment_slot = String(entry, "equipment_slot", where);
        if (entry.HasMember("health_restore")) item.health_restore = Integer(entry, "health_restore", where);
        if (entry.HasMember("attack")) item.attack = Integer(entry, "attack", where);
        if (entry.HasMember("defense")) item.defense = Integer(entry, "defense", where);
        if (entry.HasMember("move_speed_bonus")) {
            Require(entry["move_speed_bonus"].IsNumber(), where + ".move_speed_bonus must be a number");
            item.move_speed_bonus = entry["move_speed_bonus"].GetDouble();
        }
        data.items.push_back(std::move(item));
    }
    ValidateData(data);
    return data;
}
Data LoadResource(const std::string &relative) { return LoadFile(Resolve(relative)); }
const Definition *Find(const Data &data, const std::string &id) {
    const auto found = data.index.find(id);
    if (found == data.index.end() || found->second >= data.items.size()) return nullptr;
    const auto &item = data.items[found->second];
    return item.id == id ? &item : nullptr;
}
} // namespace ItemCatalog
