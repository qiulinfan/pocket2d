#include "shared/animation/SpriteAnimationAsset.h"
#include "shared/resources/ResourcePath.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
constexpr const char *kValid = R"({
  "version":1,"image":"hero.png","columns":3,"rows":2,
  "tile_width_px":4,"tile_height_px":5,
  "clips":[
    {"name":"walk","loop":true,"frames":[
      {"row":1,"column":1,"ticks":2},
      {"row":2,"column":3,"ticks":3,"event":"footstep"}]},
    {"name":"idle","loop":false,"frames":[{"row":1,"column":2,"ticks":36000}]}
  ]
})";

void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Fn> void Reject(Fn operation, const std::string &message) {
    try { operation(); }
    catch (const std::exception &) { return; }
    throw std::runtime_error("accepted invalid animation: " + message);
}

std::string UTF8(const std::filesystem::path &path) {
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}

void Write(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    Check(file.good(), "fixture write failed");
}

void WritePNG(const std::filesystem::path &path, int width, int height) {
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
        SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
    Check(image != nullptr, "fixture surface creation failed");
    Check(SDL_FillSurfaceRect(image.get(), nullptr, 0xff336699), "fixture fill failed");
    Check(IMG_SavePNG(image.get(), UTF8(path).c_str()), "fixture PNG write failed");
}

class Fixture {
public:
    std::filesystem::path base;
    std::filesystem::path root;
    explicit Fixture() : previous_root_(ResourcePath::ResourcesRootPath()) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        base = std::filesystem::temp_directory_path() / ("pocket-animation-asset-" + std::to_string(stamp));
        Check(std::filesystem::create_directory(base), "temporary directory collision");
        root = base / "project";
        std::filesystem::create_directories(root / "images" / "nested");
        std::filesystem::create_directories(root / "animations" / "nested");
        WritePNG(root / "images" / "hero.png", 12, 10);
        WritePNG(root / "images" / "nested" / "hero.png", 4, 5);
        Write(root / "animations" / "nested" / "hero.animation.json", kValid);
        ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture() {
        ResourcePath::SetResourcesRootPath(previous_root_);
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
    SpriteAnimationAsset::Data LoadText(const std::string &text) const {
        const auto path = root / "animations" / "input.animation.json";
        Write(path, text);
        return SpriteAnimationAsset::LoadFile(path, root / "images");
    }
    void RejectJSON(const std::function<void(rapidjson::Document &)> &change,
                    const std::string &reason) const {
        rapidjson::Document document;
        document.Parse(kValid);
        change(document);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        document.Accept(writer);
        Reject([&] { LoadText({buffer.GetString(), buffer.GetSize()}); }, reason);
    }
private:
    std::filesystem::path previous_root_;
};

void TestValidAndExactLookup(const Fixture &fixture) {
    const auto data = SpriteAnimationAsset::LoadResource("nested/hero.animation.json");
    Check(data.version == 1 && data.columns == 3 && data.rows == 2 && data.clips.size() == 2,
          "valid asset fields must remain intact");
    const auto *walk = SpriteAnimationAsset::FindClip(data, "walk");
    Check(walk == &data.clips.front() && walk->loop && walk->frames.size() == 2 &&
              walk->frames[0].event.empty() && walk->frames[1].event == "footstep" &&
              walk->frames[1].row == 2 && walk->frames[1].column == 3 && walk->frames[1].ticks == 3,
          "clip order, marker and one-based atlas indices must be preserved");
    Check(SpriteAnimationAsset::FindClip(data, "Walk") == nullptr &&
              SpriteAnimationAsset::FindClip(data, "missing") == nullptr,
          "clip lookup must be exact and case-sensitive");
    Check(data.resolved_image_path == std::filesystem::canonical(fixture.root / "images" / "hero.png"),
          "atlas must resolve to a canonical absolute path");
    Reject([] { SpriteAnimationAsset::LoadResource("hero.animation.json"); },
           "missing root path must not discover a nested basename");
    Reject([] { SpriteAnimationAsset::LoadResource("nested/hero"); }, "suffix is mandatory");
    auto wrong = data;
    wrong.image = "nested/hero.png";
    Reject([&] { SpriteAnimationAsset::ValidateAtlasImage(wrong, fixture.root / "images"); },
           "exact nested image dimensions must be used, never root basename fallback");
    Check(wrong.resolved_image_path == data.resolved_image_path,
          "failed atlas validation must not publish a replacement derived path");
    Check(!std::filesystem::exists(fixture.root / ".pocketengine"),
          "animation loading must not depend on or create editor grid metadata");
}

void TestStrictJSON(const Fixture &fixture) {
    fixture.RejectJSON([](auto &j) { j.RemoveMember("image"); }, "missing required field");
    fixture.RejectJSON([](auto &j) { j.AddMember("unknown", 1, j.GetAllocator()); }, "unknown root field");
    fixture.RejectJSON([](auto &j) { j.AddMember("version", 1, j.GetAllocator()); }, "duplicate root field");
    fixture.RejectJSON([](auto &j) { j["version"].SetInt(2); }, "unsupported version");
    fixture.RejectJSON([](auto &j) { j["columns"].SetDouble(3.0); }, "floating-point integer spelling");
    fixture.RejectJSON([](auto &j) { j["rows"].SetUint64(2147483648ULL); }, "out-of-range integer");
    fixture.RejectJSON([](auto &j) { j["image"].SetBool(false); }, "image wrong type");
    fixture.RejectJSON([](auto &j) { j["clips"].SetObject(); }, "clips wrong shape");
    fixture.RejectJSON([](auto &j) { j["clips"].Clear(); }, "empty clip list");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["frames"].Clear(); }, "empty clip frames");
    fixture.RejectJSON([](auto &j) { j["clips"][1]["name"].SetString("walk"); }, "duplicate clip name");
    fixture.RejectJSON([](auto &j) { j["clips"][0].AddMember("loop", false, j.GetAllocator()); }, "duplicate clip field");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["loop"].SetInt(1); }, "loop is strictly boolean");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["frames"][0].AddMember("ticks", 1, j.GetAllocator()); }, "duplicate frame field");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["frames"][0].AddMember("duration", 1, j.GetAllocator()); }, "unknown frame field");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["frames"][0].SetInt(1); }, "frame must be an object");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["frames"][0]["ticks"].SetInt(0); }, "zero duration");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["frames"][0]["ticks"].SetInt(36001); }, "duration cap");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["frames"][0]["row"].SetInt(0); }, "zero row");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["frames"][0]["column"].SetInt(4); }, "column outside grid");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["frames"][1]["event"].SetString(""); }, "explicit empty event");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["frames"][1]["event"].SetBool(false); }, "event wrong type");
    fixture.RejectJSON([](auto &j) { j["clips"][0]["name"].SetString(""); }, "empty name");
    Reject([&] { fixture.LoadText("[]"); }, "root must be an object");
    Reject([&] { fixture.LoadText(std::string(10000, '[') + std::string(10000, ']')); }, "deep malformed schema");
}

void TestBoundsAndEncoding(const Fixture &fixture) {
    auto data = fixture.LoadText(kValid);
    auto invalid = data;
    invalid.columns = std::numeric_limits<int>::max();
    invalid.rows = 2;
    Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "grid product overflow");
    invalid = data;
    invalid.tile_width_px = std::numeric_limits<int>::max();
    Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "pixel product overflow");
    invalid = data;
    invalid.rows = -1;
    Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "negative grid");
    invalid = data;
    invalid.clips.front().name.assign(129, 'a');
    Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "name byte cap");
    invalid = data;
    invalid.clips.front().frames.front().event.assign(129, 'a');
    Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "marker byte cap");
    invalid.clips.front().frames.front().event.assign(128, 'a');
    invalid.clips.front().name.assign(128, 'b');
    SpriteAnimationAsset::ValidateData(invalid);
    invalid.clips.front().name = std::string("bad\0name", 8);
    Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "embedded NUL name");
    invalid = data;
    invalid.clips.front().name = std::string("\xc0\xaf", 2);
    Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "overlong UTF-8 name");
    invalid = data;
    invalid.clips.front().frames.front().event = std::string("\xf0\x9f", 2);
    Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "truncated UTF-8 marker");
    std::string bad_json = kValid;
    bad_json.replace(bad_json.find("walk"), 4, std::string(1, static_cast<char>(255)));
    Reject([&] { fixture.LoadText(bad_json); }, "invalid UTF-8 JSON");
    fixture.RejectJSON([](auto &j) {
        j["image"].SetString("hero.png\0outside", 16, j.GetAllocator());
    }, "image embedded NUL");

    invalid = data;
    invalid.clips.clear();
    for (int i = 0; i < 256; ++i)
        invalid.clips.push_back({"clip" + std::to_string(i), false, {{1, 1, 1, {}}}});
    SpriteAnimationAsset::ValidateData(invalid);
    invalid.clips.push_back({"extra", false, {{1, 1, 1, {}}}});
    Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "clip count cap");
    invalid = data;
    invalid.clips.resize(1);
    invalid.clips.front().frames.assign(65536, {1, 1, 36000, {}});
    SpriteAnimationAsset::ValidateData(invalid); // Legal total duration exceeds signed int32.
    invalid.clips.front().frames.push_back({1, 1, 1, {}});
    Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "total frame cap");

    std::string maximum = kValid;
    maximum.resize(4 * 1024 * 1024, ' ');
    Check(fixture.LoadText(maximum).clips.size() == 2, "exact 4 MiB file must load");
    maximum.push_back(' ');
    Reject([&] { fixture.LoadText(maximum); }, "file byte cap");
}

void TestPathsAndImages(const Fixture &fixture) {
    auto data = fixture.LoadText(kValid);
    for (const std::string &path : {std::string("../hero.png"), std::string("/hero.png"),
                                    std::string("C:/hero.png"), std::string("..\\hero.png")}) {
        auto invalid = data;
        invalid.image = path;
        Reject([&] { SpriteAnimationAsset::ValidateData(invalid); }, "portable image path confinement");
    }
    Reject([] { SpriteAnimationAsset::LoadResource("../outside.animation.json"); }, "resource traversal");
    Reject([] { SpriteAnimationAsset::LoadResource("/outside.animation.json"); }, "absolute resource");
    Reject([] { SpriteAnimationAsset::LoadResource(std::string("nested/hero.animation.json\0ignored", 32)); }, "resource NUL");
    auto invalid = data;
    invalid.image = "missing.png";
    Reject([&] { SpriteAnimationAsset::ValidateAtlasImage(invalid, fixture.root / "images"); }, "missing image");
    Write(fixture.root / "images" / "broken.png", "not a decodable image");
    invalid.image = "broken.png";
    Reject([&] { SpriteAnimationAsset::ValidateAtlasImage(invalid, fixture.root / "images"); }, "image decode failure");

    const auto unicode_path = fixture.root / "images" / std::filesystem::path(u8"角色.png");
    WritePNG(unicode_path, 12, 10);
    invalid = data;
    invalid.image = UTF8(unicode_path.filename());
    invalid.clips.front().name = "行走";
    invalid.clips.front().frames.front().event = "脚步";
    SpriteAnimationAsset::ValidateAtlasImage(invalid, fixture.root / "images");
    Check(invalid.resolved_image_path == std::filesystem::canonical(unicode_path),
          "valid UTF-8 atlas names must resolve exactly");

    WritePNG(fixture.base / "outside.png", 12, 10);
    Write(fixture.base / "outside.animation.json", kValid);
    std::error_code error;
    std::filesystem::create_symlink(fixture.base / "outside.png", fixture.root / "images" / "escape.png", error);
#if defined(_WIN32)
    if (error == std::errc::operation_not_permitted || error == std::errc::permission_denied) {
        std::cout << "symlink cases unavailable without Windows symlink permission\n";
        return;
    }
#endif
    Check(!error, "unable to create image symlink fixture: " + error.message());
    invalid = data;
    invalid.image = "escape.png";
    Reject([&] { SpriteAnimationAsset::ValidateAtlasImage(invalid, fixture.root / "images"); }, "image symlink escape");
    std::filesystem::create_symlink(fixture.base / "outside.animation.json",
                                    fixture.root / "animations" / "escape.animation.json");
    Reject([] { SpriteAnimationAsset::LoadResource("escape.animation.json"); }, "resource symlink escape");
    std::filesystem::create_directory_symlink(fixture.base, fixture.root / "animations" / "outside-dir");
    Reject([] { SpriteAnimationAsset::LoadResource("outside-dir/outside.animation.json"); }, "directory symlink escape");
    std::filesystem::create_symlink(fixture.root / "animations" / "nested" / "hero.animation.json",
                                    fixture.root / "animations" / "alias.animation.json");
    Check(SpriteAnimationAsset::LoadResource("alias.animation.json").resolved_image_path == data.resolved_image_path,
          "in-root aliases may resolve to the same canonical resource");
}
// A clip may draw from its own sheet of the resource's tile size.
void TestClipSheets(const Fixture &fixture) {
    WritePNG(fixture.root / "images" / "run.png", 16, 5);   // 4 columns x 1 row of 4x5 tiles
    const auto data = fixture.LoadText(R"({
      "version":1,"image":"hero.png","columns":3,"rows":2,"tile_width_px":4,"tile_height_px":5,
      "clips":[
        {"name":"idle","loop":true,"frames":[{"row":2,"column":3,"ticks":1}]},
        {"name":"run","loop":true,"sheet":{"image":"run.png","columns":4,"rows":1},
         "frames":[{"row":1,"column":4,"ticks":2}]}]})");
    const auto &idle = data.clips[0], &run = data.clips[1];
    Check(SpriteAnimationAsset::ClipColumns(data, idle) == 3 && SpriteAnimationAsset::ClipRows(data, idle) == 2 &&
              SpriteAnimationAsset::ClipImagePath(data, idle) == UTF8(data.resolved_image_path),
          "clips without a sheet use the resource atlas");
    Check(SpriteAnimationAsset::ClipColumns(data, run) == 4 && SpriteAnimationAsset::ClipRows(data, run) == 1 &&
              run.resolved_image_path == std::filesystem::canonical(fixture.root / "images" / "run.png"),
          "a clip sheet resolves its own image and grid");
    Reject([&] { fixture.LoadText(R"({"version":1,"image":"hero.png","columns":3,"rows":2,
      "tile_width_px":4,"tile_height_px":5,"clips":[{"name":"run","loop":true,
      "sheet":{"image":"run.png","columns":5,"rows":1},"frames":[{"row":1,"column":1,"ticks":1}]}]})"); },
      "sheet pixel size must match its grid");
    Reject([&] { fixture.LoadText(R"({"version":1,"image":"hero.png","columns":3,"rows":2,
      "tile_width_px":4,"tile_height_px":5,"clips":[{"name":"run","loop":true,
      "sheet":{"image":"run.png","columns":4,"rows":1},"frames":[{"row":2,"column":1,"ticks":1}]}]})"); },
      "frames must lie inside the clip's own sheet");
    Reject([&] { fixture.LoadText(R"({"version":1,"image":"hero.png","columns":3,"rows":2,
      "tile_width_px":4,"tile_height_px":5,"clips":[{"name":"run","loop":true,
      "sheet":{"image":"run.png","columns":4,"rows":1,"extra":1},"frames":[{"row":1,"column":1,"ticks":1}]}]})"); },
      "sheet fields are exact");
}
void TestChangedAtlasRevalidates(const Fixture &fixture) {
    // Atlas sizes are remembered per file; replacing the image must be noticed.
    SpriteAnimationAsset::LoadResource("nested/hero.animation.json");
    WritePNG(fixture.root / "images" / "hero.png", 16, 10);
    Reject([] { SpriteAnimationAsset::LoadResource("nested/hero.animation.json"); },
           "a replaced atlas with other dimensions must be rejected");
    WritePNG(fixture.root / "images" / "hero.png", 12, 10);
    SpriteAnimationAsset::LoadResource("nested/hero.animation.json");
}
} // namespace

int main() {
    try {
        Check(SDL_Init(0), "SDL test initialization failed");
        {
            Fixture fixture;
            TestValidAndExactLookup(fixture);
            TestStrictJSON(fixture);
            TestBoundsAndEncoding(fixture);
            TestPathsAndImages(fixture);
            TestClipSheets(fixture);
            TestChangedAtlasRevalidates(fixture);
        }
        SDL_Quit();
        std::cout << "SpriteAnimation asset schema, bounds, UTF-8 and atlas confinement regressions passed\n";
        return 0;
    } catch (const std::exception &error) {
        SDL_Quit();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
