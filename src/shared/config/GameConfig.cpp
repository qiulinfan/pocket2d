#include "shared/config/GameConfig.h"
#include "core/Log.h"
#include "shared/resources/ResourcePath.h"
#include "shared/persistence/SaveStore.h"
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

std::filesystem::path GameConfigPath() {
    return ResourcePath::ResourcesRootPath() / "game.config";
}

std::filesystem::path RenderingConfigPath() {
    return ResourcePath::ResourcesRootPath() / "rendering.config";
}

void ReadJsonFile(const std::string &path, rapidjson::Document &out_document) {
    FILE *file_pointer = nullptr;
#ifdef _WIN32
    fopen_s(&file_pointer, path.c_str(), "rb");
#else
    file_pointer = fopen(path.c_str(), "rb");
#endif
    if (file_pointer == nullptr) {
        throw std::runtime_error("unable to open [" + path + "]");
    }
    char buffer[65536];
    rapidjson::FileReadStream stream(file_pointer, buffer, sizeof(buffer));
    out_document.ParseStream(stream);
    std::fclose(file_pointer);

    if (out_document.HasParseError() || !out_document.IsObject())
        throw std::runtime_error("invalid JSON object at [" + path + "]");
}

void ValidateUniqueMembers(const rapidjson::Document &document, const std::filesystem::path &path) {
    std::unordered_set<std::string> names;
    for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member) {
        const std::string name(member->name.GetString(), member->name.GetStringLength());
        if (!names.insert(name).second) {
            throw std::runtime_error(path.string() + ": duplicate field " + name);
        }
    }
}

void ValidateRenderingConfig(const rapidjson::Document &document, const std::filesystem::path &path) {
    ValidateUniqueMembers(document, path);
    for (const char *name : {"x_resolution", "y_resolution", "clear_color_r", "clear_color_g", "clear_color_b"}) {
        if (!document.HasMember(name)) continue;
        const auto &value = document[name];
        if (!value.IsInt()) throw std::runtime_error(path.string() + ": " + name + " must be an integer");
        const bool resolution = std::string(name).find("resolution") != std::string::npos;
        if ((resolution && value.GetInt() <= 0) ||
            (!resolution && (value.GetInt() < 0 || value.GetInt() > 255))) {
            throw std::runtime_error(path.string() + ": " + name +
                                     (resolution ? " must be positive" : " must be in [0, 255]"));
        }
    }
    if (document.HasMember("zoom_factor")) {
        const auto &value = document["zoom_factor"];
        if (!value.IsNumber() || !std::isfinite(value.GetDouble()) ||
            value.GetDouble() <= 0.0 || value.GetDouble() > std::numeric_limits<float>::max()) {
            throw std::runtime_error(path.string() + ": zoom_factor must be a finite positive float");
        }
    }
}

bool TryReadJsonObjectFile(const std::filesystem::path &path,
                           rapidjson::Document &out_document) {
    if (!std::filesystem::exists(path)) {
        out_document.SetObject();
        return true;
    }

    FILE *file_pointer = nullptr;
#ifdef _WIN32
    fopen_s(&file_pointer, path.string().c_str(), "rb");
#else
    file_pointer = fopen(path.string().c_str(), "rb");
#endif
    if (file_pointer == nullptr) {
        return false;
    }

    char buffer[65536];
    rapidjson::FileReadStream stream(file_pointer, buffer, sizeof(buffer));
    out_document.ParseStream(stream);
    std::fclose(file_pointer);
    if (out_document.HasParseError() || !out_document.IsObject()) {
        return false;
    }
    return true;
}

void UpsertStringMember(rapidjson::Document &document, const char *key,
                        const std::string &value) {
    rapidjson::Value key_value(key, document.GetAllocator());
    rapidjson::Value string_value(value.c_str(), document.GetAllocator());
    if (document.HasMember(key)) {
        document[key].SetString(value.c_str(), document.GetAllocator());
        return;
    }
    document.AddMember(key_value.Move(), string_value.Move(),
                       document.GetAllocator());
}

void UpsertIntMember(rapidjson::Document &document, const char *key, int value) {
    rapidjson::Value key_value(key, document.GetAllocator());
    rapidjson::Value int_value(value);
    if (document.HasMember(key)) {
        document[key].SetInt(value);
        return;
    }
    document.AddMember(key_value.Move(), int_value.Move(),
                       document.GetAllocator());
}

void UpsertFloatMember(rapidjson::Document &document, const char *key,
                       float value) {
    rapidjson::Value key_value(key, document.GetAllocator());
    rapidjson::Value float_value(value);
    if (document.HasMember(key)) {
        document[key].SetFloat(value);
        return;
    }
    document.AddMember(key_value.Move(), float_value.Move(),
                       document.GetAllocator());
}

bool WriteJsonFile(const std::filesystem::path &path,
                   const rapidjson::Document &document) {
    std::error_code directory_error;
    if (!path.parent_path().empty() &&
        !std::filesystem::exists(path.parent_path(), directory_error)) {
        std::filesystem::create_directories(path.parent_path(), directory_error);
        if (directory_error) return false;
    }

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    document.Accept(writer);

    std::ofstream output_file(path, std::ios::out | std::ios::trunc);
    if (!output_file.is_open()) {
        return false;
    }
    output_file << buffer.GetString() << std::endl;
    return output_file.good();
}

} // namespace

// Read game.config and rendering.config into one lightweight shared value
// object. Engine owns the behavior; this type only describes startup data.
GameConfigData GameConfig::Read() {
    GameConfigData config;

    // Shared startup config is intentionally simple: the runtime and editor
    // both consume the same read-only values, then diverge into their own
    // session state once bootstrapping is complete.
    const std::filesystem::path resources_root =
        ResourcePath::ResourcesRootPath();
    const std::filesystem::path game_config_path = GameConfigPath();
    const std::filesystem::path rendering_config_path = RenderingConfigPath();

    if (!std::filesystem::exists(resources_root) ||
        !std::filesystem::is_directory(resources_root)) {
        Log::Warning("config", "project folder missing [" + resources_root.string() + "]; using default config");
        return config;
    }
    if (!std::filesystem::exists(game_config_path)) {
        Log::Warning("config", "project game.config missing [" + game_config_path.string() + "]; using default config");
        return config;
    }

    rapidjson::Document game_config;
    ReadJsonFile(game_config_path.string(), game_config);
    ValidateUniqueMembers(game_config, game_config_path);
    for (const char *field : {"game_title", "initial_scene", "save_namespace"}) {
        if (game_config.HasMember(field) && !game_config[field].IsString()) {
            throw std::runtime_error(game_config_path.string() + ": " + field + " must be a string");
        }
    }

    if (game_config.HasMember("save_namespace")) {
        const auto &value = game_config["save_namespace"];
        config.save_namespace.assign(value.GetString(), value.GetStringLength());
        if (!config.save_namespace.empty() && !SaveStore::IsValidIdentifier(config.save_namespace))
            throw std::runtime_error("game.config: invalid save_namespace");
    }

    if (game_config.HasMember("game_title") &&
        game_config["game_title"].IsString()) {
        config.game_title = game_config["game_title"].GetString();
    }

    if (game_config.HasMember("initial_scene") &&
        game_config["initial_scene"].IsString()) {
        config.initial_scene_name = game_config["initial_scene"].GetString();
    }

    if (game_config.HasMember("splash_screen")) {
        if (!game_config["splash_screen"].IsBool())
            throw std::runtime_error(game_config_path.string() + ": splash_screen must be true or false");
        config.splash_screen = game_config["splash_screen"].GetBool();
    }
    if (game_config.HasMember("splash_seconds")) {
        const auto &value = game_config["splash_seconds"];
        if (!value.IsNumber() || value.GetFloat() < 0.5f || value.GetFloat() > 10.0f)
            throw std::runtime_error(game_config_path.string() + ": splash_seconds must be a number in [0.5, 10]");
        config.splash_seconds = value.GetFloat();
    }
    if (game_config.HasMember("show_fps")) {
        if (!game_config["show_fps"].IsBool())
            throw std::runtime_error(game_config_path.string() + ": show_fps must be true or false");
        config.show_fps = game_config["show_fps"].GetBool();
    }
    if (game_config.HasMember("max_fps")) {
        const auto &value = game_config["max_fps"];
        if (!value.IsInt() || value.GetInt() < 0 || value.GetInt() > 1000)
            throw std::runtime_error(game_config_path.string() + ": max_fps must be an integer in [0, 1000] (0: no limit)");
        config.max_fps = value.GetInt();
    }
    if (game_config.HasMember("vsync")) {
        if (!game_config["vsync"].IsBool())
            throw std::runtime_error(game_config_path.string() + ": vsync must be true or false");
        config.vsync = game_config["vsync"].GetBool();
    }
    if (game_config.HasMember("frame_interpolation")) {
        if (!game_config["frame_interpolation"].IsBool())
            throw std::runtime_error(game_config_path.string() + ": frame_interpolation must be true or false");
        config.frame_interpolation = game_config["frame_interpolation"].GetBool();
    }

    if (std::filesystem::exists(rendering_config_path)) {
        rapidjson::Document rendering_config;
        ReadJsonFile(rendering_config_path.string(), rendering_config);
        ValidateRenderingConfig(rendering_config, rendering_config_path);

        if (rendering_config.HasMember("x_resolution") &&
            rendering_config["x_resolution"].IsInt()) {
            config.window_width = rendering_config["x_resolution"].GetInt();
        }
        if (rendering_config.HasMember("y_resolution") &&
            rendering_config["y_resolution"].IsInt()) {
            config.window_height = rendering_config["y_resolution"].GetInt();
        }
        if (rendering_config.HasMember("clear_color_r") &&
            rendering_config["clear_color_r"].IsInt()) {
            const int value = rendering_config["clear_color_r"].GetInt();
            if (value >= 0 && value <= 255) config.clear_color_r = value;
        }
        if (rendering_config.HasMember("clear_color_g") &&
            rendering_config["clear_color_g"].IsInt()) {
            const int value = rendering_config["clear_color_g"].GetInt();
            if (value >= 0 && value <= 255) config.clear_color_g = value;
        }
        if (rendering_config.HasMember("clear_color_b") &&
            rendering_config["clear_color_b"].IsInt()) {
            const int value = rendering_config["clear_color_b"].GetInt();
            if (value >= 0 && value <= 255) config.clear_color_b = value;
        }
        if (rendering_config.HasMember("zoom_factor") &&
            rendering_config["zoom_factor"].IsNumber()) {
            const float zoom = rendering_config["zoom_factor"].GetFloat();
            if (zoom > 0.0f) config.zoom_factor = zoom;
        }
    }

    return config;
}

bool GameConfig::Write(const GameConfigData &config) {
    if (!std::isfinite(config.zoom_factor) ||
        (!config.save_namespace.empty() && !SaveStore::IsValidIdentifier(config.save_namespace))) return false;
    const std::filesystem::path game_config_path = GameConfigPath();
    const std::filesystem::path rendering_config_path = RenderingConfigPath();

    rapidjson::Document game_config;
    if (!TryReadJsonObjectFile(game_config_path, game_config)) {
        Log::Error("config", "unable to read [" + game_config_path.string() + "]");
        return false;
    }
    if (!game_config.IsObject()) {
        game_config.SetObject();
    }

    rapidjson::Document rendering_config;
    if (!TryReadJsonObjectFile(rendering_config_path, rendering_config)) {
        Log::Error("config", "unable to read [" + rendering_config_path.string() + "]");
        return false;
    }
    if (!rendering_config.IsObject()) {
        rendering_config.SetObject();
    }

    UpsertStringMember(game_config, "game_title", config.game_title);
    UpsertStringMember(game_config, "initial_scene", config.initial_scene_name);
    UpsertStringMember(game_config, "save_namespace", config.save_namespace);
    if (config.splash_screen || game_config.HasMember("splash_screen")) {
        if (game_config.HasMember("splash_screen")) game_config["splash_screen"].SetBool(config.splash_screen);
        else game_config.AddMember("splash_screen", config.splash_screen, game_config.GetAllocator());
        UpsertFloatMember(game_config, "splash_seconds", std::clamp(config.splash_seconds, 0.5f, 10.0f));
    }
    if (config.show_fps || game_config.HasMember("show_fps")) {
        if (game_config.HasMember("show_fps")) game_config["show_fps"].SetBool(config.show_fps);
        else game_config.AddMember("show_fps", config.show_fps, game_config.GetAllocator());
    }
    if (config.max_fps != 0 || game_config.HasMember("max_fps"))
        UpsertIntMember(game_config, "max_fps", std::clamp(config.max_fps, 0, 1000));
    if (config.vsync || game_config.HasMember("vsync")) {
        if (game_config.HasMember("vsync")) game_config["vsync"].SetBool(config.vsync);
        else game_config.AddMember("vsync", config.vsync, game_config.GetAllocator());
    }
    if (!config.frame_interpolation || game_config.HasMember("frame_interpolation")) {
        if (game_config.HasMember("frame_interpolation"))
            game_config["frame_interpolation"].SetBool(config.frame_interpolation);
        else game_config.AddMember("frame_interpolation", config.frame_interpolation, game_config.GetAllocator());
    }

    UpsertIntMember(rendering_config, "x_resolution",
                    std::max(1, config.window_width));
    UpsertIntMember(rendering_config, "y_resolution",
                    std::max(1, config.window_height));
    UpsertIntMember(rendering_config, "clear_color_r",
                    std::max(0, std::min(255, config.clear_color_r)));
    UpsertIntMember(rendering_config, "clear_color_g",
                    std::max(0, std::min(255, config.clear_color_g)));
    UpsertIntMember(rendering_config, "clear_color_b",
                    std::max(0, std::min(255, config.clear_color_b)));
    UpsertFloatMember(rendering_config, "zoom_factor",
                      config.zoom_factor > 0.0f ? config.zoom_factor : 1.0f);

    if (!WriteJsonFile(game_config_path, game_config)) {
        Log::Error("config", "unable to write [" + game_config_path.string() + "]");
        return false;
    }
    if (!WriteJsonFile(rendering_config_path, rendering_config)) {
        Log::Error("config", "unable to write [" + rendering_config_path.string() + "]");
        return false;
    }
    return true;
}
