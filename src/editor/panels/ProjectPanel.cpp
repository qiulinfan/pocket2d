#include "editor/panels/ProjectPanel.h"
#include "editor/core/EditorDragDrop.h"
#include "editor/core/EditorTheme.h"
#include "imgui.h"
#include "shared/config/SpritesheetConfig.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3/SDL.h"
#include "SDL3_image/SDL_image.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// Project panel: a read-only browser for the active project root.
// It never mutates runtime or scene-document state directly. Instead, it
// surfaces user intent (open scene / open external editor) through
// ProjectPanelResult so the host can decide what to do next.

namespace EditorPanels {
namespace {

struct ProjectEntry {
    std::filesystem::path path;
    bool is_directory = false;
};

struct DirectoryListing {
    std::vector<ProjectEntry> entries;
    bool loaded = false;
    bool is_directory = false;
    int last_refresh_frame = -1;
};

struct DirectoryListingCache {
    std::filesystem::path resources_root;
    std::unordered_map<std::string, DirectoryListing> directories;
};

struct ProjectPanelState {
    std::filesystem::path selected_directory;
    std::filesystem::path selected_entry;
    float tile_size = 80.0f;  // icon tile width, set by the size slider
};

// File type classification drives icon choice plus double-click behavior.
enum class EntryKind {
    Directory,
    Audio,
    Lua,
    Config,
    Template,
    Image,
    Font,
    Scene,
    GenericFile
};

ImVec4 Alpha(const ImVec4 &color, float alpha) { return ImVec4(color.x, color.y, color.z, alpha); }

struct EntryVisualStyle {
    const char *badge = "";  // type badge on file icons
    ImVec4 color = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
};

struct TileRenderResult {
    bool clicked = false;
    bool double_clicked = false;
    bool expand_toggled = false;
};

struct TextureDrawRect {
    ImVec2 min;
    ImVec2 max;
    bool valid = false;
};

struct ImageTextureCache {
    SDL_Renderer *renderer = nullptr;
    std::unordered_map<std::string, SDL_Texture *> textures;
};

struct SpritesheetPopupState {
    bool open_requested = false;
    std::filesystem::path resources_root;
    std::filesystem::path image_path;
    int rows = 1;
    int columns = 1;
};

constexpr int kDirectoryListingRefreshIntervalFrames = 60;

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return text;
}

/*
Read one directory from disk and return entries in a stable UI order:
directories first, then files, both alphabetically.
*/
std::vector<ProjectEntry> LoadSortedEntries(
    const std::filesystem::path &directory_path, bool &out_is_directory) {
    std::vector<ProjectEntry> entries;
    out_is_directory = false;

    std::error_code status_error;
    if (!std::filesystem::exists(directory_path, status_error) ||
        status_error ||
        !std::filesystem::is_directory(directory_path, status_error) ||
        status_error) {
        return entries;
    }
    out_is_directory = true;

    std::error_code iterate_error;
    // list directories first, then files, both sorted alphabetically
    for (std::filesystem::directory_iterator it(directory_path, iterate_error), end;
         !iterate_error && it != end; it.increment(iterate_error)) {
        const std::filesystem::directory_entry &entry = *it;
        const std::string filename = entry.path().filename().string();
        if (!filename.empty() && filename.front() == '.') {
            continue;
        }
        std::error_code entry_error;
        ProjectEntry project_entry;
        project_entry.path = entry.path().lexically_normal();
        project_entry.is_directory = entry.is_directory(entry_error);
        entries.emplace_back(std::move(project_entry));
    }

    std::sort(entries.begin(), entries.end(),
              [](const ProjectEntry &a, const ProjectEntry &b) {
                  if (a.is_directory != b.is_directory) {
                      return a.is_directory > b.is_directory;
                  }
                  return a.path.filename().string() < b.path.filename().string();
              });
    return entries;
}

EntryKind ClassifyEntry(const ProjectEntry &entry) {
    if (entry.is_directory) return EntryKind::Directory;

    const std::string extension = ToLower(entry.path.extension().string());
    if (extension == ".scene") return EntryKind::Scene;
    if (extension == ".lua") return EntryKind::Lua;
    if (extension == ".template") return EntryKind::Template;
    if (extension == ".config") return EntryKind::Config;
    if (extension == ".wav" || extension == ".mp3" || extension == ".ogg" ||
        extension == ".flac" || extension == ".m4a" || extension == ".aac") {
        return EntryKind::Audio;
    }
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".bmp" || extension == ".tga" || extension == ".gif" ||
        extension == ".webp") {
        return EntryKind::Image;
    }
    if (extension == ".ttf" || extension == ".otf") return EntryKind::Font;
    return EntryKind::GenericFile;
}

// map a logical entry kind to its tile visuals
EntryVisualStyle GetEntryVisualStyle(EntryKind kind) {
    switch (kind) {
    case EntryKind::Directory: return {"", ImVec4(0.93f, 0.74f, 0.33f, 1.0f)};
    case EntryKind::Audio: return {"AUDIO", ImVec4(0.38f, 0.80f, 0.52f, 1.0f)};
    case EntryKind::Lua: return {"LUA", ImVec4(0.40f, 0.62f, 0.98f, 1.0f)};
    case EntryKind::Config: return {"CFG", ImVec4(0.62f, 0.66f, 0.74f, 1.0f)};
    case EntryKind::Template: return {"TPL", ImVec4(0.74f, 0.55f, 0.96f, 1.0f)};
    case EntryKind::Image: return {"IMG", ImVec4(0.98f, 0.60f, 0.30f, 1.0f)};
    case EntryKind::Font: return {"FONT", ImVec4(0.92f, 0.47f, 0.55f, 1.0f)};
    case EntryKind::Scene: return {"SCENE", ImVec4(0.30f, 0.80f, 0.78f, 1.0f)};
    default: return {"FILE", ImVec4(0.62f, 0.66f, 0.74f, 1.0f)};
    }
}

// Vector icons for the Project browser, drawn to fit a square.
ImU32 Shade(const ImVec4 &color, float factor, float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4(color.x * factor, color.y * factor, color.z * factor, alpha));
}

// A flat folder: a back panel with its tab and a lighter front.
void DrawFolderIcon(ImDrawList *draw_list, const ImVec2 &min, const ImVec2 &max, const ImVec4 &color) {
    const float side = std::min(max.x - min.x, max.y - min.y);
    const float w = side * 0.86f, h = side * 0.68f;
    const ImVec2 o((min.x + max.x - w) * 0.5f, (min.y + max.y - h) * 0.5f);
    const float r = side * 0.07f;
    draw_list->AddRectFilled(o, ImVec2(o.x + w * 0.42f, o.y + h * 0.3f), Shade(color, 0.78f), r);
    draw_list->AddRectFilled(ImVec2(o.x, o.y + h * 0.12f), ImVec2(o.x + w, o.y + h), Shade(color, 0.78f), r);
    draw_list->AddRectFilled(ImVec2(o.x, o.y + h * 0.26f), ImVec2(o.x + w, o.y + h), Shade(color, 1.0f), r);
}

// A page with a folded corner, a few text lines and a coloured type badge.
void DrawFileIcon(ImDrawList *draw_list, const ImVec2 &min, const ImVec2 &max, const ImVec4 &color,
                  const char *badge) {
    const EditorPalette &theme = EditorTheme::Current();
    const float side = std::min(max.x - min.x, max.y - min.y);
    const float h = side * 0.9f, w = h * 0.76f;
    const float l = (min.x + max.x - w) * 0.5f, t = (min.y + max.y - h) * 0.5f;
    const float r = l + w, b = t + h, fold = w * 0.3f, radius = side * 0.06f;
    const ImVec4 page = theme.light ? ImVec4(0.98f, 0.98f, 0.99f, 1.0f) : ImVec4(0.84f, 0.86f, 0.91f, 1.0f);

    draw_list->PathClear();
    draw_list->PathArcToFast(ImVec2(l + radius, t + radius), radius, 6, 9);
    draw_list->PathLineTo(ImVec2(r - fold, t));
    draw_list->PathLineTo(ImVec2(r, t + fold));
    draw_list->PathArcToFast(ImVec2(r - radius, b - radius), radius, 0, 3);
    draw_list->PathArcToFast(ImVec2(l + radius, b - radius), radius, 3, 6);
    draw_list->PathFillConvex(Shade(page, 1.0f));
    draw_list->AddTriangleFilled(ImVec2(r - fold, t), ImVec2(r - fold, t + fold), ImVec2(r, t + fold),
                                 Shade(page, 0.72f));
    if (theme.light) {
        draw_list->PathClear();
        draw_list->PathArcToFast(ImVec2(l + radius, t + radius), radius, 6, 9);
        draw_list->PathLineTo(ImVec2(r - fold, t));
        draw_list->PathLineTo(ImVec2(r, t + fold));
        draw_list->PathArcToFast(ImVec2(r - radius, b - radius), radius, 0, 3);
        draw_list->PathArcToFast(ImVec2(l + radius, b - radius), radius, 3, 6);
        draw_list->PathStroke(ImGui::GetColorU32(theme.border), ImDrawFlags_Closed, 1.0f);
    }
    const float line = std::max(1.0f, side * 0.035f);
    for (int i = 0; i < 3; ++i) {
        const float y = t + h * (0.24f + 0.12f * static_cast<float>(i));
        const float end = i == 0 ? r - fold - w * 0.08f : r - w * (i == 2 ? 0.34f : 0.16f);
        draw_list->AddRectFilled(ImVec2(l + w * 0.16f, y), ImVec2(end, y + line), Shade(page, 0.7f), line);
    }

    ImFont *font = ImGui::GetFont();
    const float font_size = std::max(8.0f, std::min(ImGui::GetFontSize() * 0.68f, side * 0.17f));
    const ImVec2 text = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, badge);
    const ImVec2 badge_min(l + w * 0.12f, b - h * 0.16f - text.y - 4.0f);
    const ImVec2 badge_max(std::min(r + w * 0.1f, badge_min.x + text.x + 8.0f), badge_min.y + text.y + 4.0f);
    draw_list->AddRectFilled(badge_min, badge_max, Shade(color, 1.0f), 4.0f);
    draw_list->AddText(font, font_size, ImVec2(badge_min.x + 4.0f, badge_min.y + 2.0f),
                       IM_COL32(255, 255, 255, 255), badge);
}

// A small folder before a tree row's name (rows are labelled with FolderRowLabel).
void DrawTreeFolder() {
    const ImVec2 row_min = ImGui::GetItemRectMin(), row_max = ImGui::GetItemRectMax();
    const float size = ImGui::GetFontSize();
    const float x = row_min.x + ImGui::GetTreeNodeToLabelSpacing();
    const float y = (row_min.y + row_max.y - size) * 0.5f;
    DrawFolderIcon(ImGui::GetWindowDrawList(), ImVec2(x, y), ImVec2(x + size, y + size),
                   GetEntryVisualStyle(EntryKind::Directory).color);
}

// Bridge Project-panel entry kinds into the external-editor configuration enum
EditorExternalFileType ToExternalFileType(EntryKind kind) {
    switch (kind) {
    case EntryKind::Audio:
        return EditorExternalFileType::Audio;
    case EntryKind::Lua:
        return EditorExternalFileType::Lua;
    case EntryKind::Config:
        return EditorExternalFileType::Config;
    case EntryKind::Template:
        return EditorExternalFileType::Template;
    case EntryKind::Image:
        return EditorExternalFileType::Image;
    case EntryKind::Font:
        return EditorExternalFileType::Font;
    case EntryKind::Scene:
        return EditorExternalFileType::Scene;
    case EntryKind::Directory:
    case EntryKind::GenericFile:
    default:
        return EditorExternalFileType::Generic;
    }
}


ImageTextureCache &GetImageTextureCache() {
    static ImageTextureCache cache;
    return cache;
}

DirectoryListingCache &GetDirectoryListingCache() {
    static DirectoryListingCache cache;
    return cache;
}

ProjectPanelState &GetProjectPanelState() {
    static ProjectPanelState state;
    return state;
}

SpritesheetPopupState &GetSpritesheetPopupState() {
    static SpritesheetPopupState state;
    return state;
}

std::unordered_map<std::string, bool> &GetExpandedSpritesheetState() {
    static std::unordered_map<std::string, bool> state;
    return state;
}

std::string DirectoryCacheKey(const std::filesystem::path &directory_path) {
    return directory_path.lexically_normal().generic_string();
}

void InvalidateDirectoryListings() {
    GetDirectoryListingCache().directories.clear();
}

void DestroyTextureCache(ImageTextureCache &cache) {
    for (const auto &entry : cache.textures) {
        if (entry.second != nullptr) {
            SDL_DestroyTexture(entry.second);
        }
    }
    cache.textures.clear();
}


const DirectoryListing &GetCachedDirectoryListing(
    const std::filesystem::path &directory_path, bool force_refresh = false) {
    DirectoryListingCache &cache = GetDirectoryListingCache();
    const int frame_count = ImGui::GetFrameCount();
    const std::string cache_key = DirectoryCacheKey(directory_path);
    DirectoryListing &listing = cache.directories[cache_key];
    const bool refresh_due =
        !listing.loaded ||
        frame_count - listing.last_refresh_frame >=
            kDirectoryListingRefreshIntervalFrames;
    if (force_refresh || refresh_due) {
        listing.entries =
            LoadSortedEntries(directory_path.lexically_normal(),
                              listing.is_directory);
        listing.loaded = true;
        listing.last_refresh_frame = frame_count;
    }
    return listing;
}

void ResetProjectPanelForRoot(const std::filesystem::path &resources_root) {
    DirectoryListingCache &cache = GetDirectoryListingCache();
    const std::filesystem::path normalized_root = resources_root.lexically_normal();
    if (cache.resources_root == normalized_root) return;

    cache.resources_root = normalized_root;
    cache.directories.clear();
    ProjectPanelState &state = GetProjectPanelState();
    state.selected_directory.clear();
    state.selected_entry.clear();
    GetExpandedSpritesheetState().clear();
}


void ResetImageTextureCacheIfRendererChanged(SDL_Renderer *renderer) {
    ImageTextureCache &cache = GetImageTextureCache();
    if (cache.renderer == renderer) return;

    DestroyTextureCache(cache);
    cache.renderer = renderer;
}


SDL_Texture *GetImageTexture(SDL_Renderer *renderer, const std::filesystem::path &image_path) {
    if (renderer == nullptr || image_path.empty()) return nullptr;

    ResetImageTextureCacheIfRendererChanged(renderer);
    ImageTextureCache &cache = GetImageTextureCache();
    const std::string cache_key = image_path.lexically_normal().string();

    const auto found = cache.textures.find(cache_key);
    if (found != cache.textures.end()) {
        return found->second;
    }

    SDL_Texture *texture = IMG_LoadTexture(renderer, cache_key.c_str());
    cache.textures.emplace(cache_key, texture);
    return texture;
}

TextureDrawRect BuildTextureFitRect(SDL_Texture *texture, const ImVec2 &min_point,
                                    const ImVec2 &max_point) {
    TextureDrawRect draw_rect;
    if (texture == nullptr) return draw_rect;

    float texture_width = 0.0f;
    float texture_height = 0.0f;
    SDL_GetTextureSize(texture, &texture_width, &texture_height);
    if (texture_width <= 0.0f || texture_height <= 0.0f) return draw_rect;

    const float available_width = max_point.x - min_point.x;
    const float available_height = max_point.y - min_point.y;
    if (available_width <= 0.0f || available_height <= 0.0f) return draw_rect;

    const float scale = std::min(available_width / texture_width, available_height / texture_height);
    const float draw_width = texture_width * scale;
    const float draw_height = texture_height * scale;
    draw_rect.min = ImVec2(min_point.x + (available_width - draw_width) * 0.5f,
                           min_point.y + (available_height - draw_height) * 0.5f);
    draw_rect.max = ImVec2(draw_rect.min.x + draw_width, draw_rect.min.y + draw_height);
    draw_rect.valid = true;
    return draw_rect;
}

bool IsNonTrivialSpritesheet(const SpritesheetGridSpec &spec) {
    return spec.rows > 1 || spec.columns > 1;
}

void DrawTextureRegionFitCentered(ImDrawList *draw_list, SDL_Texture *texture,
                                  const ImVec2 &min_point,
                                  const ImVec2 &max_point,
                                  const ImVec2 &uv_min,
                                  const ImVec2 &uv_max) {
    if (draw_list == nullptr || texture == nullptr) return;

    float texture_width = 0.0f;
    float texture_height = 0.0f;
    SDL_GetTextureSize(texture, &texture_width, &texture_height);
    const float region_width =
        texture_width * std::max(0.0f, uv_max.x - uv_min.x);
    const float region_height =
        texture_height * std::max(0.0f, uv_max.y - uv_min.y);
    if (region_width <= 0.0f || region_height <= 0.0f) return;

    const float available_width = max_point.x - min_point.x;
    const float available_height = max_point.y - min_point.y;
    if (available_width <= 0.0f || available_height <= 0.0f) return;

    const float scale =
        std::min(available_width / region_width, available_height / region_height);
    const float draw_width = region_width * scale;
    const float draw_height = region_height * scale;
    const ImVec2 draw_min(min_point.x + (available_width - draw_width) * 0.5f,
                          min_point.y + (available_height - draw_height) * 0.5f);
    const ImVec2 draw_max(draw_min.x + draw_width, draw_min.y + draw_height);

    draw_list->AddImage(ImTextureRef((ImTextureID)(intptr_t)texture), draw_min,
                        draw_max, uv_min, uv_max);
}

void DrawTextureFitCentered(ImDrawList *draw_list, SDL_Texture *texture,
                            const ImVec2 &min_point,
                            const ImVec2 &max_point) {
    if (draw_list == nullptr || texture == nullptr) return;

    const TextureDrawRect draw_rect = BuildTextureFitRect(texture, min_point, max_point);
    if (!draw_rect.valid) return;

    draw_list->AddImage(ImTextureRef((ImTextureID)(intptr_t)texture), draw_rect.min, draw_rect.max);
}

void DrawSpritesheetGridOverlay(ImDrawList *draw_list,
                                const TextureDrawRect &draw_rect, int rows,
                                int columns) {
    if (draw_list == nullptr || !draw_rect.valid) return;
    rows = std::max(rows, 1);
    columns = std::max(columns, 1);

    const ImU32 line_color = IM_COL32(120, 210, 255, 210);
    const float width = draw_rect.max.x - draw_rect.min.x;
    const float height = draw_rect.max.y - draw_rect.min.y;

    for (int row = 1; row < rows; ++row) {
        const float y = draw_rect.min.y + height * (static_cast<float>(row) / static_cast<float>(rows));
        draw_list->AddLine(ImVec2(draw_rect.min.x, y), ImVec2(draw_rect.max.x, y),
                           line_color, 1.5f);
    }
    for (int column = 1; column < columns; ++column) {
        const float x = draw_rect.min.x + width * (static_cast<float>(column) / static_cast<float>(columns));
        draw_list->AddLine(ImVec2(x, draw_rect.min.y), ImVec2(x, draw_rect.max.y),
                           line_color, 1.5f);
    }
    draw_list->AddRect(draw_rect.min, draw_rect.max, line_color, 0.0f, 0, 1.5f);
}

void DrawDisclosureGlyph(ImDrawList *draw_list, const ImVec2 &min_point,
                         const ImVec2 &max_point, bool expanded,
                         bool hovered) {
    if (draw_list == nullptr) return;

    const EditorPalette &theme = EditorTheme::Current();
    const ImU32 fill_color = ImGui::GetColorU32(hovered ? theme.accent : theme.button);
    draw_list->AddRectFilled(min_point, max_point, fill_color, 4.0f);

    const ImVec2 center((min_point.x + max_point.x) * 0.5f,
                        (min_point.y + max_point.y) * 0.5f);
    const ImU32 glyph_color = ImGui::GetColorU32(hovered ? theme.badge_text : theme.text);
    if (expanded) {
        draw_list->AddTriangleFilled(
            ImVec2(center.x - 4.5f, center.y - 2.5f),
            ImVec2(center.x + 4.5f, center.y - 2.5f),
            ImVec2(center.x, center.y + 4.5f), glyph_color);
        return;
    }
    draw_list->AddTriangleFilled(
        ImVec2(center.x - 2.5f, center.y - 4.5f),
        ImVec2(center.x - 2.5f, center.y + 4.5f),
        ImVec2(center.x + 4.5f, center.y), glyph_color);
}

void OpenSpritesheetPopupForImage(const std::filesystem::path &resources_root,
                                  const std::filesystem::path &image_path) {
    SpritesheetPopupState &popup_state = GetSpritesheetPopupState();
    const SpritesheetGridSpec spec =
        SpritesheetConfig::ReadForImagePath(resources_root, image_path);

    popup_state.resources_root = resources_root.lexically_normal();
    popup_state.image_path = image_path.lexically_normal();
    popup_state.rows = spec.rows;
    popup_state.columns = spec.columns;
    popup_state.open_requested = true;
}

void RenderSpritesheetPopup(SDL_Renderer *renderer) {
    SpritesheetPopupState &popup_state = GetSpritesheetPopupState();
    if (popup_state.open_requested) {
        ImGui::OpenPopup("Spritesheet Editor");
        popup_state.open_requested = false;
    }

    ImGui::SetNextWindowSize(ImVec2(860.0f, 760.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Spritesheet Editor", nullptr,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    SDL_Texture *texture = GetImageTexture(renderer, popup_state.image_path);
    if (texture == nullptr) {
        ImGui::TextUnformatted("Unable to load this image.");
    } else {
        float texture_width_f = 0.0f;
        float texture_height_f = 0.0f;
        SDL_GetTextureSize(texture, &texture_width_f, &texture_height_f);
        const int texture_width = static_cast<int>(texture_width_f);
        const int texture_height = static_cast<int>(texture_height_f);

        ImGui::Text("Image: %s", popup_state.image_path.filename().string().c_str());
        ImGui::TextDisabled("%d x %d px", texture_width, texture_height);
        ImGui::Separator();

        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Rows", &popup_state.rows, 1, 4)) {
            popup_state.rows = std::max(popup_state.rows, 1);
            SpritesheetConfig::WriteForImagePath(
                popup_state.resources_root, popup_state.image_path,
                {popup_state.rows, popup_state.columns});
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("Columns", &popup_state.columns, 1, 4)) {
            popup_state.columns = std::max(popup_state.columns, 1);
            SpritesheetConfig::WriteForImagePath(
                popup_state.resources_root, popup_state.image_path,
                {popup_state.rows, popup_state.columns});
        }

        const int sprite_width = std::max(1, texture_width / std::max(popup_state.columns, 1));
        const int sprite_height = std::max(1, texture_height / std::max(popup_state.rows, 1));
        ImGui::TextDisabled("Each sprite: %d x %d px | Total sprites: %d",
                            sprite_width, sprite_height,
                            popup_state.rows * popup_state.columns);
        ImGui::Separator();

        const float preview_height = std::max(260.0f, ImGui::GetContentRegionAvail().y - 48.0f);
        ImGui::BeginChild("spritesheet_preview_child",
                          ImVec2(0.0f, preview_height), true,
                          ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 preview_min = ImGui::GetCursorScreenPos();
        const ImVec2 preview_size = ImGui::GetContentRegionAvail();
        const ImVec2 preview_max(preview_min.x + preview_size.x,
                                 preview_min.y + preview_size.y);
        ImGui::InvisibleButton("spritesheet_preview_canvas", preview_size);

        ImDrawList *draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(preview_min, preview_max,
                                 ImGui::GetColorU32(EditorTheme::Current().frame), 8.0f);
        const TextureDrawRect draw_rect = BuildTextureFitRect(
            texture, ImVec2(preview_min.x + 12.0f, preview_min.y + 12.0f),
            ImVec2(preview_max.x - 12.0f, preview_max.y - 12.0f));
        if (draw_rect.valid) {
            draw_list->AddImage(ImTextureRef((ImTextureID)(intptr_t)texture),
                                draw_rect.min, draw_rect.max);
            DrawSpritesheetGridOverlay(draw_list, draw_rect, popup_state.rows,
                                       popup_state.columns);
        }
        ImGui::EndChild();
    }

    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// Scene files are handled by the editor itself instead of an external app.
bool IsSceneFile(const ProjectEntry &entry) {
    return ClassifyEntry(entry) == EntryKind::Scene;
}

// Tree rows leave room before the name for a folder glyph (DrawTreeFolder).
std::string FolderRowLabel(const std::string &name) {
    const float room = ImGui::GetFontSize() * 1.25f;
    const int spaces = static_cast<int>(std::ceil(room / std::max(1.0f, ImGui::CalcTextSize(" ").x)));
    return std::string(static_cast<std::size_t>(spaces), ' ') + name;
}

std::string BuildDirectoryCaption(const std::filesystem::path &resources_root,
                                  const std::filesystem::path &directory_path) {
    const std::filesystem::path normalized_root = resources_root.lexically_normal();
    const std::filesystem::path normalized_directory =
        directory_path.lexically_normal();
    if (normalized_directory == normalized_root) {
        return "Project/";
    }

    if (!ResourcePath::IsPathWithinDirectory(normalized_directory,
                                             normalized_root)) {
        return "Project/";
    }
    const std::filesystem::path relative_path =
        ResourcePath::NormalizeRelativePath(
            normalized_directory.lexically_relative(normalized_root));
    if (relative_path.empty()) return "Project/";
    return "Project/" + relative_path.generic_string();
}

std::string TruncateTextToWidth(const std::string &text, float max_width) {
    if (text.empty()) return text;
    if (ImGui::CalcTextSize(text.c_str()).x <= max_width) {
        return text;
    }

    const std::string ellipsis = "...";
    std::string clipped = text;
    while (!clipped.empty()) {
        clipped.pop_back();
        const std::string candidate = clipped + ellipsis;
        if (ImGui::CalcTextSize(candidate.c_str()).x <= max_width) {
            return candidate;
        }
    }
    return ellipsis;
}

bool BuildDragResourceName(const std::filesystem::path &entry_path,
                           const std::filesystem::path &resource_root,
                           const std::string &expected_extension,
                           std::string &out_resource_name) {
    const std::filesystem::path normalized_entry = entry_path.lexically_normal();
    const std::filesystem::path normalized_root = resource_root.lexically_normal();
    if (!ResourcePath::IsPathWithinDirectory(normalized_entry,
                                             normalized_root)) {
        return false;
    }

    std::filesystem::path relative_path =
        ResourcePath::NormalizeRelativePath(
            normalized_entry.lexically_relative(normalized_root));
    if (relative_path.empty()) {
        return false;
    }

    if (!expected_extension.empty() &&
        ToLower(relative_path.extension().string()) != expected_extension) {
        return false;
    }

    relative_path.replace_extension();
    relative_path = ResourcePath::NormalizeRelativePath(relative_path);
    if (relative_path.empty()) {
        return false;
    }

    out_resource_name = relative_path.generic_string();
    return !out_resource_name.empty();
}

bool BuildLuaComponentTypeName(const std::filesystem::path &entry_path,
                               const std::filesystem::path &resources_root,
                               std::string &out_component_type) {
    if (!ResourcePath::IsPathWithinDirectory(
            entry_path, (resources_root / "component_types").lexically_normal())) {
        return false;
    }

    if (ToLower(entry_path.extension().string()) != ".lua") {
        return false;
    }

    // Runtime currently registers Lua components by filename stem, not by
    // directory-qualified path. Drag-and-drop needs to match that registry.
    out_component_type = entry_path.stem().string();
    return !out_component_type.empty();
}

bool BuildImageResourceName(const std::filesystem::path &entry_path,
                            const std::filesystem::path &resources_root,
                            std::string &out_resource_name) {
    return BuildDragResourceName(
        entry_path, (resources_root / "images").lexically_normal(), "",
        out_resource_name);
}

bool BuildAudioResourceName(const std::filesystem::path &entry_path,
                            const std::filesystem::path &resources_root,
                            std::string &out_resource_name) {
    return BuildDragResourceName(
        entry_path, (resources_root / "audio").lexically_normal(), "",
        out_resource_name);
}

void CopySpritePayloadResourceName(EditorDragDrop::SpriteAssetPayload &payload,
                                   const std::string &resource_name) {
    std::snprintf(payload.image_resource_name,
                  sizeof(payload.image_resource_name), "%s",
                  resource_name.c_str());
}

void RenderEntryDragSourceIfSupported(const ProjectEntry &entry,
                                      EntryKind entry_kind,
                                      const std::filesystem::path &resources_root) {
    std::string payload_value;
    const char *payload_type = nullptr;
    const char *payload_label = nullptr;

    // Only built-in project asset kinds that map cleanly into editor actions
    // become drag sources in this first pass.
    if (entry_kind == EntryKind::Lua &&
        BuildLuaComponentTypeName(entry.path, resources_root, payload_value)) {
        payload_type = EditorDragDrop::kLuaComponentPayload;
        payload_label = "Lua Component";
    } else if (entry_kind == EntryKind::Image &&
               BuildImageResourceName(entry.path, resources_root,
                                      payload_value)) {
        payload_type = EditorDragDrop::kImageAssetPayload;
        payload_label = "Image";
    } else if (entry_kind == EntryKind::Audio &&
               BuildAudioResourceName(entry.path, resources_root,
                                      payload_value)) {
        payload_type = EditorDragDrop::kAudioAssetPayload;
        payload_label = "Audio";
    } else if (entry_kind == EntryKind::Template &&
               BuildDragResourceName(
                   entry.path,
                   (resources_root / "actor_templates").lexically_normal(),
                   ".template", payload_value)) {
        payload_type = EditorDragDrop::kActorTemplatePayload;
        payload_label = "Actor Template";
    }

    if (payload_type == nullptr) {
        return;
    }

    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(payload_type, payload_value.c_str(),
                                  payload_value.size() + 1);
        ImGui::Text("%s", payload_label);
        ImGui::TextDisabled("%s", payload_value.c_str());
        ImGui::EndDragDropSource();
    }
}

// Render the left-side directory navigator recursively
void RenderDirectoryTree(const std::filesystem::path &resources_root,
                         const std::filesystem::path &directory_path,
                         std::filesystem::path &selected_directory) {
    const DirectoryListing &listing = GetCachedDirectoryListing(directory_path);
    if (!listing.is_directory) return;
    const std::filesystem::path normalized_root =
        resources_root.lexically_normal();

    for (const ProjectEntry &entry : listing.entries) {
        if (!entry.is_directory) {
            continue;
        }

        const std::filesystem::path relative_path =
            ResourcePath::NormalizeRelativePath(
                entry.path.lexically_normal().lexically_relative(
                    normalized_root));
        if (relative_path.empty() ||
            !ResourcePath::IsPathWithinDirectory(entry.path,
                                                 normalized_root)) {
            continue;
        }

        ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (entry.path == selected_directory) {
            node_flags |= ImGuiTreeNodeFlags_Selected;
        }

        const std::string node_id = relative_path.string();
        const std::string label = FolderRowLabel(entry.path.filename().string());
        const bool is_open = ImGui::TreeNodeEx(node_id.c_str(), node_flags, "%s", label.c_str());
        DrawTreeFolder();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            selected_directory = entry.path;
        }

        if (is_open) {
            RenderDirectoryTree(resources_root, entry.path, selected_directory);
            ImGui::TreePop();
        }
    }
}


// Report click state; navigation handled by the parent grid so all entry actions stay centralized
TileRenderResult RenderEntryTile(const ProjectEntry &entry,
                                 EntryKind entry_kind,
                                 const EntryVisualStyle &style,
                                 const std::filesystem::path &resources_root,
                                 SDL_Renderer *renderer, bool selected,
                                 const SpritesheetGridSpec &spritesheet_spec,
                                 bool spritesheet_expanded,
                                 float tile_width,
                                 float tile_height) {
    TileRenderResult result;
    const std::string entry_id = entry.path.string();
    ImGui::PushID(entry_id.c_str());

    const ImVec2 tile_size(tile_width, tile_height);
    const ImVec2 tile_min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("project_entry_tile", tile_size);
    const bool hovered = ImGui::IsItemHovered();
    result.clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    result.double_clicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    RenderEntryDragSourceIfSupported(entry, entry_kind, resources_root);

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const ImVec2 tile_max(tile_min.x + tile_size.x, tile_min.y + tile_size.y);

    // No frame at rest: a soft rounded highlight on hover and selection.
    const EditorPalette &theme = EditorTheme::Current();
    if (selected || hovered) {
        const ImVec4 highlight = selected ? theme.selection : Alpha(ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered), 0.7f);
        draw_list->AddRectFilled(tile_min, tile_max, ImGui::GetColorU32(highlight), 8.0f);
    }

    const float pad = std::max(4.0f, tile_width * 0.08f);
    const float name_height = ImGui::GetTextLineHeight();
    const ImVec2 icon_min(tile_min.x + pad, tile_min.y + pad);
    const ImVec2 icon_max(tile_max.x - pad, tile_max.y - name_height - pad * 1.2f);
    const bool spritesheet_expandable =
        entry_kind == EntryKind::Image && IsNonTrivialSpritesheet(spritesheet_spec);
    SDL_Texture *image_texture = entry_kind == EntryKind::Image ? GetImageTexture(renderer, entry.path) : nullptr;
    if (image_texture != nullptr) {
        draw_list->AddRectFilled(icon_min, icon_max, ImGui::GetColorU32(theme.frame), 6.0f);
        DrawTextureFitCentered(draw_list, image_texture, ImVec2(icon_min.x + 3.0f, icon_min.y + 3.0f),
                               ImVec2(icon_max.x - 3.0f, icon_max.y - 3.0f));
    } else if (entry_kind == EntryKind::Directory) {
        DrawFolderIcon(draw_list, icon_min, icon_max, style.color);
    } else {
        DrawFileIcon(draw_list, icon_min, icon_max, style.color, style.badge);
    }

    const std::string file_name = entry.path.filename().string();
    const std::string visible_name = TruncateTextToWidth(file_name, tile_width - 6.0f);
    const ImVec2 name_size = ImGui::CalcTextSize(visible_name.c_str());
    const ImVec2 name_pos((tile_min.x + tile_max.x - name_size.x) * 0.5f, tile_max.y - name_height - pad * 0.5f);
    draw_list->AddText(name_pos, ImGui::GetColorU32(theme.text), visible_name.c_str());
    if (hovered && visible_name != file_name) ImGui::SetTooltip("%s", file_name.c_str());

    if (spritesheet_expandable) {
        const ImVec2 toggle_min(icon_max.x - 18.0f, icon_min.y + 2.0f);
        const ImVec2 toggle_max(icon_max.x - 2.0f, icon_min.y + 18.0f);
        const bool toggle_hovered =
            ImGui::IsMouseHoveringRect(toggle_min, toggle_max);
        if (toggle_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            result.expand_toggled = true;
            result.double_clicked = false;
        }
        DrawDisclosureGlyph(draw_list, toggle_min, toggle_max,
                            spritesheet_expanded, toggle_hovered);

        const std::string badge_text =
            std::to_string(spritesheet_spec.rows) + "x" +
            std::to_string(spritesheet_spec.columns);
        const ImVec2 badge_text_size = ImGui::CalcTextSize(badge_text.c_str());
        const ImVec2 badge_min(icon_min.x + 3.0f,
                               icon_max.y - badge_text_size.y - 9.0f);
        const ImVec2 badge_max(badge_min.x + badge_text_size.x + 10.0f,
                               badge_min.y + badge_text_size.y + 6.0f);
        draw_list->AddRectFilled(badge_min, badge_max,
                                 ImGui::GetColorU32(Alpha(theme.header, 0.9f)), 4.0f);
        draw_list->AddText(ImVec2(badge_min.x + 5.0f, badge_min.y + 3.0f),
                           ImGui::GetColorU32(theme.text), badge_text.c_str());
    }

    ImGui::PopID();
    return result;
}

void RenderSpritesheetSpriteTile(SDL_Texture *texture, int rows, int columns,
                                 int row_index, int column_index,
                                 const std::string &image_resource_name,
                                 float tile_width, float tile_height) {
    const ImVec2 tile_min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("spritesheet_sprite_tile",
                           ImVec2(tile_width, tile_height));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 tile_max(tile_min.x + tile_width, tile_min.y + tile_height);
    ImDrawList *draw_list = ImGui::GetWindowDrawList();

    const EditorPalette &theme = EditorTheme::Current();
    draw_list->AddRectFilled(
        tile_min, tile_max,
        ImGui::GetColorU32(hovered ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered) : theme.tile), 7.0f);
    draw_list->AddRect(tile_min, tile_max, ImGui::GetColorU32(theme.border), 7.0f);

    const ImVec2 preview_min(tile_min.x + 8.0f, tile_min.y + 8.0f);
    const ImVec2 preview_max(tile_max.x - 8.0f, tile_min.y + tile_height - 28.0f);
    draw_list->AddRectFilled(preview_min, preview_max, ImGui::GetColorU32(theme.frame),
                             4.0f);

    const ImVec2 uv_min(
        static_cast<float>(column_index) / static_cast<float>(columns),
        static_cast<float>(row_index) / static_cast<float>(rows));
    const ImVec2 uv_max(
        static_cast<float>(column_index + 1) / static_cast<float>(columns),
        static_cast<float>(row_index + 1) / static_cast<float>(rows));
    DrawTextureRegionFitCentered(draw_list, texture, preview_min, preview_max,
                                 uv_min, uv_max);

    if (!image_resource_name.empty() && ImGui::BeginDragDropSource()) {
        EditorDragDrop::SpriteAssetPayload payload;
        CopySpritePayloadResourceName(payload, image_resource_name);
        payload.row = row_index + 1;
        payload.column = column_index + 1;
        ImGui::SetDragDropPayload(EditorDragDrop::kSpriteAssetPayload, &payload,
                                  sizeof(payload));
        ImGui::Text("Sprite");
        ImGui::TextDisabled("%s [%d,%d]", image_resource_name.c_str(),
                            payload.row, payload.column);
        ImGui::EndDragDropSource();
    }

    const std::string label = "[" + std::to_string(row_index + 1) + "," +
                              std::to_string(column_index + 1) + "]";
    const ImVec2 label_size = ImGui::CalcTextSize(label.c_str());
    draw_list->AddText(
        ImVec2((tile_min.x + tile_max.x - label_size.x) * 0.5f,
               tile_max.y - label_size.y - 8.0f),
        ImGui::GetColorU32(theme.text), label.c_str());
}

ImVec2 ComputeSpritesheetSpritePreviewSize(SDL_Texture *texture, int rows,
                                           int columns) {
    if (texture == nullptr) return ImVec2(72.0f, 72.0f);

    float texture_width = 0.0f;
    float texture_height = 0.0f;
    SDL_GetTextureSize(texture, &texture_width, &texture_height);
    const float sprite_width =
        static_cast<float>(texture_width) / static_cast<float>(std::max(columns, 1));
    const float sprite_height =
        static_cast<float>(texture_height) / static_cast<float>(std::max(rows, 1));
    const float max_side = std::max(sprite_width, sprite_height);
    if (max_side <= 0.0f) return ImVec2(72.0f, 72.0f);

    constexpr float kMaxPreviewSide = 64.0f;
    const float scale = kMaxPreviewSide / max_side;
    return ImVec2(std::max(24.0f, sprite_width * scale),
                  std::max(24.0f, sprite_height * scale));
}

void RenderExpandedSpritesheetTileGrid(const ProjectEntry &entry,
                                       const SpritesheetGridSpec &spec,
                                       const std::filesystem::path &resources_root,
                                       SDL_Renderer *renderer) {
    SDL_Texture *texture = GetImageTexture(renderer, entry.path);
    if (texture == nullptr) return;

    std::string image_resource_name;
    if (!BuildImageResourceName(entry.path, resources_root,
                                image_resource_name)) {
        image_resource_name.clear();
    }

    const int rows = std::max(spec.rows, 1);
    const int columns = std::max(spec.columns, 1);
    const int sprite_count = rows * columns;
    if (sprite_count <= 1) return;

    ImGui::Indent(18.0f);
    ImGui::TextDisabled("Sprites");
    const ImVec2 preview_size =
        ComputeSpritesheetSpritePreviewSize(texture, rows, columns);
    const float sprite_tile_width = std::max(84.0f, preview_size.x + 16.0f);
    const float sprite_tile_height = std::max(96.0f, preview_size.y + 36.0f);
    constexpr float kSpriteTileSpacing = 8.0f;
    const float available_width = ImGui::GetContentRegionAvail().x;
    const int sprite_columns =
        std::max(1, static_cast<int>((available_width + kSpriteTileSpacing) /
                                     (sprite_tile_width + kSpriteTileSpacing)));

    int layout_column_index = 0;
    for (int sprite_index = 0; sprite_index < sprite_count; ++sprite_index) {
        if (layout_column_index > 0) {
            ImGui::SameLine(0.0f, kSpriteTileSpacing);
        }

        const int row_index = sprite_index / columns;
        const int column_index = sprite_index % columns;
        ImGui::PushID(sprite_index);
        RenderSpritesheetSpriteTile(texture, rows, columns, row_index,
                                    column_index, image_resource_name,
                                    sprite_tile_width,
                                    sprite_tile_height);
        ImGui::PopID();
        layout_column_index = (layout_column_index + 1) % sprite_columns;
    }
    ImGui::Unindent(18.0f);
}

// Render the right-side tile grid for the selected directory.
/*
Double-click behavior:
directory -> navigate, scene -> request open in editor
other file -> request external editor open.
*/
void RenderDirectoryGrid(const std::filesystem::path &resources_root,
                         const std::filesystem::path &directory_path,
                         std::filesystem::path &selected_directory,
                         std::filesystem::path &selected_entry,
                         SDL_Renderer *renderer,
                         ProjectPanelResult &result) {
    const DirectoryListing &listing = GetCachedDirectoryListing(directory_path);
    if (!listing.is_directory) {
        ImGui::TextDisabled("This folder is not available.");
        return;
    }
    if (listing.entries.empty()) {
        ImGui::TextDisabled("This folder is empty.");
        return;
    }

    const float kTileWidth = GetProjectPanelState().tile_size;
    const float kTileHeight = kTileWidth + ImGui::GetTextLineHeight() + 4.0f;
    constexpr float kTileSpacing = 6.0f;

    const float available_width = ImGui::GetContentRegionAvail().x;
    const int columns = std::max( 1, static_cast<int>((available_width + kTileSpacing) / (kTileWidth + kTileSpacing)));

    /*
    Delay directory navigation until after the loop 
    so tile rendering and selection logic use a stable current-directory snapshot this frame
    */
    std::filesystem::path next_directory;
    int column_index = 0;
    for (const ProjectEntry &entry : listing.entries) {
        if (column_index > 0) {
            ImGui::SameLine(0.0f, kTileSpacing);
        }

        const EntryKind entry_kind = ClassifyEntry(entry);
        const EntryVisualStyle style = GetEntryVisualStyle(entry_kind);
        const SpritesheetGridSpec spritesheet_spec =
            (entry_kind == EntryKind::Image)
                ? SpritesheetConfig::ReadForImagePath(resources_root, entry.path)
                : SpritesheetGridSpec{};
        std::unordered_map<std::string, bool> &expanded_state =
            GetExpandedSpritesheetState();
        const std::string expanded_key = entry.path.lexically_normal().string();
        const bool is_spritesheet_expanded =
            expanded_state.find(expanded_key) != expanded_state.end() &&
            expanded_state[expanded_key];
        const bool is_selected = selected_entry == entry.path;
        const TileRenderResult tile_result =
            RenderEntryTile(entry, entry_kind, style, resources_root, renderer,
                            is_selected, spritesheet_spec,
                            is_spritesheet_expanded, kTileWidth, kTileHeight);

        if (tile_result.clicked) {
            selected_entry = entry.path;
        }
        if (tile_result.expand_toggled && IsNonTrivialSpritesheet(spritesheet_spec)) {
            expanded_state[expanded_key] = !is_spritesheet_expanded;
        }
        if (tile_result.double_clicked) {
            if (entry.is_directory) {
                next_directory = entry.path;
            } else if (IsSceneFile(entry)) {
                result.open_scene_requested = true;
                result.requested_scene_path = entry.path.lexically_normal();
            } else if (entry_kind == EntryKind::Image) {
                OpenSpritesheetPopupForImage(resources_root, entry.path);
            } else {
                result.open_external_editor_requested = true;
                result.requested_external_file_type = ToExternalFileType(entry_kind);
                result.requested_external_file_path = entry.path.lexically_normal();
                result.requested_external_project_root = resources_root.lexically_normal();
            }
        }
        column_index = (column_index + 1) % columns;

        const bool now_expanded =
            IsNonTrivialSpritesheet(spritesheet_spec) &&
            expanded_state.find(expanded_key) != expanded_state.end() &&
            expanded_state[expanded_key];
        if (!now_expanded) continue;

        if (column_index != 0) {
            ImGui::NewLine();
            column_index = 0;
        }
        RenderExpandedSpritesheetTileGrid(entry, spritesheet_spec,
                                          resources_root, renderer);
        ImGui::Spacing();
    }

    if (!next_directory.empty()) {
        selected_directory = next_directory;
        selected_entry.clear();
    }
}

} // namespace

// Render a read-only project browser tree for the active project root.
ProjectPanelResult RenderProjectPanel( const std::filesystem::path &resources_root, SDL_Renderer *renderer) {
    // Project panel owns only UI-navigation state. The actual file-system state
    // lives on disk, and open requests are returned to the host as a result.
    ProjectPanelResult result;
    ResetProjectPanelForRoot(resources_root);
    ResetImageTextureCacheIfRendererChanged(renderer);

    const bool window_visible = ImGui::Begin("Project");
    if (!window_visible) {
        ImGui::End();
        return result;
    }

    const std::filesystem::path normalized_root = resources_root.lexically_normal();
    if (ImGui::Button("Refresh")) {
        InvalidateDirectoryListings();
        DestroyTextureCache(GetImageTextureCache());
    }
    // Icon size, like the slider at the bottom of Unity's Project window.
    const float slider_width = 120.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - slider_width));
    ImGui::SetNextItemWidth(slider_width);
    ImGui::SliderFloat("##project_icon_size", &GetProjectPanelState().tile_size, 56.0f, 128.0f, "");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Icon size");
    ImGui::Separator();

    const DirectoryListing &root_listing =
        GetCachedDirectoryListing(normalized_root);
    if (!root_listing.is_directory) {
        ImGui::TextUnformatted("Project folder not found.");
        ImGui::End();
        return result;
    }

    ProjectPanelState &state = GetProjectPanelState();
    // If the remembered directory disappeared, fall back to the project root.
    if (state.selected_directory.empty() ||
        !ResourcePath::IsPathWithinDirectory(state.selected_directory,
                                             normalized_root)) {
        state.selected_directory = normalized_root;
        state.selected_entry.clear();
    } else {
        const DirectoryListing &selected_listing =
            GetCachedDirectoryListing(state.selected_directory);
        if (!selected_listing.is_directory) {
            state.selected_directory = normalized_root;
            state.selected_entry.clear();
        }
    }

    const std::filesystem::path previous_directory = state.selected_directory;
    const ImGuiTableFlags layout_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("project_layout", 2, layout_flags)) {
        ImGui::TableSetupColumn("Tree", ImGuiTableColumnFlags_WidthStretch, 0.32f);
        ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch, 0.68f);

        // Left: folder tree navigator.
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Folders");
        ImGui::Separator();
        ImGui::BeginChild("project_tree", ImVec2(0.0f, 0.0f), false);
        ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (state.selected_directory == normalized_root) {
            root_flags |= ImGuiTreeNodeFlags_Selected;
        }
        const bool root_open = ImGui::TreeNodeEx("project_root", root_flags, "%s", FolderRowLabel("Project").c_str());
        DrawTreeFolder();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            state.selected_directory = normalized_root;
        }
        if (root_open) {
            RenderDirectoryTree(normalized_root, normalized_root,
                                state.selected_directory);
            ImGui::TreePop();
        }
        ImGui::EndChild();

        // Right: Unity-like tile grid for entries in the current folder.
        ImGui::TableNextColumn();
        ImGui::Text("Current Folder: %s",
                    BuildDirectoryCaption(normalized_root,
                                          state.selected_directory)
                        .c_str());
        ImGui::TextDisabled("click folder to enter, double-click scene to open.");
        ImGui::Separator();
        ImGui::BeginChild("project_grid", ImVec2(0.0f, 0.0f), false);
        RenderDirectoryGrid(normalized_root, state.selected_directory,
                            state.selected_directory, state.selected_entry,
                            renderer, result);
        ImGui::EndChild();

        ImGui::EndTable();
    }

    if (state.selected_directory != previous_directory) {
        state.selected_entry.clear();
    }

    RenderSpritesheetPopup(renderer);
    ImGui::End();
    return result;
}

void InvalidateProjectPanelCache() {
    InvalidateDirectoryListings();

    ProjectPanelState &state = GetProjectPanelState();
    state.selected_directory.clear();
    state.selected_entry.clear();

    GetExpandedSpritesheetState().clear();
    GetSpritesheetPopupState() = SpritesheetPopupState{};
    DestroyTextureCache(GetImageTextureCache());
}

}
