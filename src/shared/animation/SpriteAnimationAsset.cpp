#include "shared/animation/SpriteAnimationAsset.h"
#include "shared/resources/ImageSize.h"
#include "shared/resources/ResourcePath.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/memorystream.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace {
using Json = rapidjson::Value;
constexpr std::size_t kMaxFileBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxClips = 256;
constexpr std::size_t kMaxFrames = 65536;
constexpr std::size_t kMaxLabelBytes = 128;
constexpr int kMaxFrameTicks = 36000;

void Require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error("animation: " + message);
}

bool IsUTF8(const std::string &text) {
    rapidjson::MemoryStream stream(text.data(), text.size());
    while (stream.Tell() < text.size()) {
        unsigned codepoint = 0;
        if (!rapidjson::UTF8<>::Decode(stream, &codepoint)) return false;
    }
    return true;
}

void ValidateText(const std::string &text, const std::string &label, bool bounded_label) {
    Require(!text.empty(), label + " must not be empty");
    Require(!bounded_label || text.size() <= kMaxLabelBytes,
            label + " must be 1..128 UTF-8 bytes");
    Require(text.find('\0') == std::string::npos, label + " contains a null byte");
    Require(IsUTF8(text), label + " is not valid UTF-8");
}

std::filesystem::path UTF8Path(const std::string &text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string PathText(const std::filesystem::path &path) {
    const auto utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

void RequireRelative(const std::string &text, const std::string &label) {
    ValidateText(text, label, false);
    // Use one portable spelling across native platforms; backslashes and
    // drive-prefixed names must not change interpretation on Windows.
    Require(text.find('\\') == std::string::npos,
            label + " must use '/' relative path separators");
    Require(text.size() < 2 || text[1] != ':', label + " must not contain a drive prefix");
    const auto path = UTF8Path(text);
    Require(!path.is_absolute() && !path.has_root_name() && !path.has_root_directory(),
            label + " must be a relative resource path");
    for (const auto &part : path)
        Require(part != "..", label + " must not escape its resource directory");
}

std::filesystem::path ResolveWithin(const std::filesystem::path &root,
                                    const std::string &relative, const std::string &label) {
    RequireRelative(relative, label);
    const auto canonical_root = std::filesystem::weakly_canonical(std::filesystem::absolute(root));
    const auto resolved = std::filesystem::weakly_canonical(canonical_root / UTF8Path(relative));
    Require(ResourcePath::IsPathWithinDirectory(resolved, canonical_root),
            label + " resolves outside its resource directory");
    return resolved;
}

void RequireExtension(const std::filesystem::path &path) {
    Require(PathText(path.filename()).ends_with(".animation.json"),
            "resource must end in .animation.json");
}

void ExactKeys(const Json &object, std::initializer_list<std::string_view> required,
               std::initializer_list<std::string_view> optional, const std::string &where) {
    Require(object.IsObject(), where + " must be an object");
    for (const auto key : required)
        Require(object.HasMember(key.data()), where + "." + std::string(key) + " is required");
    for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member) {
        const std::string_view name(member->name.GetString(), member->name.GetStringLength());
        Require(std::find(required.begin(), required.end(), name) != required.end() ||
                    std::find(optional.begin(), optional.end(), name) != optional.end(),
                where + ": unknown field " + std::string(name));
        // Objects have at most seven legal fields; checking earlier entries
        // avoids a heap-allocated hash set for every animation frame.
        for (auto previous = object.MemberBegin(); previous != member; ++previous) {
            const std::string_view previous_name(previous->name.GetString(), previous->name.GetStringLength());
            Require(previous_name != name, where + ": duplicate field " + std::string(name));
        }
    }
}

int Integer(const Json &object, const char *key, const std::string &where) {
    Require(object[key].IsInt(), where + "." + key + " must be a signed 32-bit integer");
    return object[key].GetInt();
}

std::string String(const Json &object, const char *key, const std::string &where) {
    Require(object[key].IsString(), where + "." + key + " must be a string");
    return {object[key].GetString(), object[key].GetStringLength()};
}

std::string ReadBoundedFile(const std::filesystem::path &path) {
    Require(std::filesystem::is_regular_file(path), "resource is not a regular file: " + PathText(path));
    const auto file_size = std::filesystem::file_size(path);
    Require(file_size <= kMaxFileBytes, "resource exceeds 4 MiB: " + PathText(path));
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), "cannot open resource: " + PathText(path));
    std::string text;
    text.reserve(static_cast<std::size_t>(file_size));
    std::array<char, 8192> chunk;
    while (input) {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        Require(count <= kMaxFileBytes - text.size(), "resource exceeds 4 MiB while reading");
        text.append(chunk.data(), count);
    }
    Require(input.eof() && !input.bad(), "cannot read resource: " + PathText(path));
    return text;
}

void RequireBoundedDepth(const std::string &text) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (const char ch : text) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') {
            // The version-1 schema needs five levels. Reject deeper garbage
            // before the JSON DOM (including its destructor) sees it.
            Require(++depth <= 16, "JSON nesting exceeds the supported schema");
        } else if ((ch == '}' || ch == ']') && depth != 0) --depth;
    }
}
} // namespace

namespace SpriteAnimationAsset {
void ValidateData(const Data &data) {
    Require(data.version == 1, "unsupported version (expected 1)");
    RequireRelative(data.image, "image");
    Require(data.columns > 0 && data.rows > 0 && data.tile_width_px > 0 && data.tile_height_px > 0,
            "grid and tile pixel dimensions must be positive integers");
    const auto count = static_cast<std::int64_t>(data.columns) * data.rows;
    const auto width = static_cast<std::int64_t>(data.columns) * data.tile_width_px;
    const auto height = static_cast<std::int64_t>(data.rows) * data.tile_height_px;
    Require(count <= std::numeric_limits<int>::max() && width <= std::numeric_limits<int>::max() &&
                height <= std::numeric_limits<int>::max(),
            "grid or pixel dimensions overflow the supported integer range");
    Require(!data.clips.empty() && data.clips.size() <= kMaxClips, "clips must contain 1..256 clips");
    std::unordered_set<std::string> names;
    std::size_t total_frames = 0;
    for (const auto &clip : data.clips) {
        ValidateText(clip.name, "clip name", true);
        Require(names.insert(clip.name).second, "duplicate clip name: " + clip.name);
        Require(!clip.frames.empty(), "clip " + clip.name + " must contain at least one frame");
        Require(clip.frames.size() <= kMaxFrames - total_frames, "total frame count exceeds 65536");
        total_frames += clip.frames.size();
        if (!clip.image.empty()) {
            RequireRelative(clip.image, "clip " + clip.name + " sheet.image");
            Require(clip.columns > 0 && clip.rows > 0, "clip " + clip.name + ": sheet grid must be positive");
            Require(static_cast<std::int64_t>(clip.columns) * data.tile_width_px <= std::numeric_limits<int>::max() &&
                        static_cast<std::int64_t>(clip.rows) * data.tile_height_px <= std::numeric_limits<int>::max(),
                    "clip " + clip.name + ": sheet dimensions overflow the supported integer range");
        }
        const int columns = ClipColumns(data, clip), rows = ClipRows(data, clip);
        for (const auto &frame : clip.frames) {
            Require(frame.row >= 1 && frame.row <= rows && frame.column >= 1 && frame.column <= columns,
                    "clip " + clip.name + ": frame cell lies outside the atlas grid");
            Require(frame.ticks >= 1 && frame.ticks <= kMaxFrameTicks,
                    "clip " + clip.name + ": frame ticks must be in [1,36000]");
            if (!frame.event.empty()) ValidateText(frame.event, "frame event", true);
        }
    }
}

void ValidateAtlasImage(Data &data, const std::filesystem::path &images_root) {
    ValidateData(data);
    const auto check = [&](const std::string &image, int columns, int rows, const std::string &label) {
        const auto path = ResolveWithin(images_root, image, label);
        Require(std::filesystem::is_regular_file(path), "atlas image missing: " + PathText(path));
        const auto image_size = ImageSize::Decoded(path);
        Require(image_size.error.empty(), "cannot decode atlas " + PathText(path) + ": " + image_size.error);
        const auto width = image_size.width, height = image_size.height;
        Require(width == static_cast<std::int64_t>(columns) * data.tile_width_px &&
                    height == static_cast<std::int64_t>(rows) * data.tile_height_px,
                "atlas pixel dimensions must exactly match grid and tile pixel dimensions: " + PathText(path));
        return path;
    };
    const auto atlas = check(data.image, data.columns, data.rows, "image");
    for (auto &clip : data.clips)
        if (!clip.image.empty()) clip.resolved_image_path = check(clip.image, clip.columns, clip.rows, "sheet.image");
    data.resolved_image_path = atlas;
}

Data LoadFile(const std::filesystem::path &path, const std::filesystem::path &images_root) {
    RequireExtension(path);
    const std::string text = ReadBoundedFile(path);
    RequireBoundedDepth(text);
    rapidjson::Document document;
    document.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag>(text.data(), text.size());
    Require(!document.HasParseError(), "invalid JSON in " + PathText(path) + ": " +
            rapidjson::GetParseError_En(document.GetParseError()));
    ExactKeys(document, {"version", "image", "columns", "rows", "tile_width_px", "tile_height_px", "clips"}, {}, "asset");
    Data data;
    data.version = Integer(document, "version", "asset");
    Require(data.version == 1, "unsupported version (expected 1)");
    data.image = String(document, "image", "asset");
    data.columns = Integer(document, "columns", "asset");
    data.rows = Integer(document, "rows", "asset");
    data.tile_width_px = Integer(document, "tile_width_px", "asset");
    data.tile_height_px = Integer(document, "tile_height_px", "asset");
    const auto &clips = document["clips"];
    Require(clips.IsArray() && clips.Size() >= 1 && clips.Size() <= kMaxClips,
            "clips must be an array containing 1..256 clips");
    std::size_t total_frames = 0;
    data.clips.reserve(clips.Size());
    for (const auto &value : clips.GetArray()) {
        ExactKeys(value, {"name", "loop", "frames"}, {"sheet"}, "clip");
        Clip clip;
        clip.name = String(value, "name", "clip");
        if (value.HasMember("sheet")) {
            const auto &sheet = value["sheet"];
            ExactKeys(sheet, {"image", "columns", "rows"}, {}, "clip.sheet");
            clip.image = String(sheet, "image", "clip.sheet");
            clip.columns = Integer(sheet, "columns", "clip.sheet");
            clip.rows = Integer(sheet, "rows", "clip.sheet");
            Require(!clip.image.empty(), "clip.sheet.image must not be empty");
        }
        Require(value["loop"].IsBool(), "clip.loop must be boolean");
        clip.loop = value["loop"].GetBool();
        const auto &frames = value["frames"];
        Require(frames.IsArray() && !frames.Empty(), "clip.frames must be a nonempty array");
        Require(frames.Size() <= kMaxFrames - total_frames, "total frame count exceeds 65536");
        total_frames += frames.Size();
        clip.frames.reserve(frames.Size());
        for (const auto &entry : frames.GetArray()) {
            ExactKeys(entry, {"row", "column", "ticks"}, {"event"}, "frame");
            Frame frame;
            frame.row = Integer(entry, "row", "frame");
            frame.column = Integer(entry, "column", "frame");
            frame.ticks = Integer(entry, "ticks", "frame");
            if (entry.HasMember("event")) {
                frame.event = String(entry, "event", "frame");
                ValidateText(frame.event, "frame.event", true);
            }
            clip.frames.push_back(std::move(frame));
        }
        data.clips.push_back(std::move(clip));
    }
    ValidateAtlasImage(data, images_root);
    return data;
}

Data LoadResource(const std::string &relative) {
    const auto path = ResolveWithin(ResourcePath::ResourceSubdirectory("animations"), relative, "animation resource");
    return LoadFile(path, ResourcePath::ResourceSubdirectory("images"));
}

int ClipColumns(const Data &data, const Clip &clip) { return clip.image.empty() ? data.columns : clip.columns; }
int ClipRows(const Data &data, const Clip &clip) { return clip.image.empty() ? data.rows : clip.rows; }
std::string ClipImagePath(const Data &data, const Clip &clip) {
    if (clip.image.empty())
        return data.resolved_image_path.empty() ? data.image : PathText(data.resolved_image_path);
    return clip.resolved_image_path.empty() ? clip.image : PathText(clip.resolved_image_path);
}

const Clip *FindClip(const Data &data, const std::string &name) {
    const auto found = std::find_if(data.clips.begin(), data.clips.end(),
                                   [&](const Clip &clip) { return clip.name == name; });
    return found == data.clips.end() ? nullptr : &*found;
}
} // namespace SpriteAnimationAsset
