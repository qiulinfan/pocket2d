#include "rendering/Renderer.h"
#include "shared/config/SpritesheetConfig.h"
#include "core/Profile.h"
#include "core/Log.h"
#include "core/World.h"
#include "core/Engine.h"
#include "rendering/SDLRenderHelper.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "particles/ParticleManager.h"
#include "scene/Scene.h"
#include "glm/glm.hpp"
#include "SDL3_image/SDL_image.h"
#include <algorithm>
#include <bit>
#include <mutex>
#include <deque>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace {

constexpr float kPixelsPerUnit = 100.0f;
constexpr Uint32 kTextCacheUnusedLifetimeMs = 3000;
constexpr std::size_t kMaxTextCacheEntries = 512;

float SafeOutputScale(float scale) {
    return scale > 0.0f ? scale : 1.0f;
}

std::unordered_set<std::string> g_renderer_warning_keys;
int g_renderer_warning_count = 0;
std::string g_renderer_last_warning;

void LogRendererWarningOnce(const std::string &key,
                            const std::string &message) {
    if (g_renderer_warning_keys.find(key) != g_renderer_warning_keys.end()) {
        return;
    }
    g_renderer_warning_keys.insert(key);
    g_renderer_last_warning = message;
    ++g_renderer_warning_count;
    Log::Warning("render", g_renderer_last_warning);
}

std::string ResolveFontPath(const std::string &font_name) {
    return ResourcePath::ResolveResourcePath(
        ResourcePath::ResourceSubdirectory("fonts"), font_name, {".ttf"},
        Scene::GetActiveSceneSubdirectory());
}

std::string ResolveSystemFontPath() {
    const std::filesystem::path system_font =
        ResourcePath::EngineSystemFontsRoot() / "system.ttf";
    return std::filesystem::exists(system_font) ? system_font.string() : "";
}

std::string ResolveImagePath(const std::string &image_name) {
    const std::filesystem::path exact(image_name);
    if (exact.is_absolute()) {
        const auto root = std::filesystem::weakly_canonical(std::filesystem::absolute(ResourcePath::ResourceSubdirectory("images")));
        const auto resolved = std::filesystem::weakly_canonical(exact);
        return ResourcePath::IsPathWithinDirectory(resolved, root) &&
                       std::filesystem::is_regular_file(resolved) ? resolved.string() : "";
    }
    return ResourcePath::ResolveResourcePath(
        ResourcePath::ResourceSubdirectory("images"), image_name,
        {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".webp"},
        Scene::GetActiveSceneSubdirectory());
}

int ClampByte(int value) {
    return std::clamp(value, 0, 255);
}

Uint8 ClampByteU8(int value) {
    return static_cast<Uint8>(ClampByte(value));
}

// Maps a float to an unsigned integer with the same ordering.
std::uint32_t SortableFloatBits(float value) {
    if (!(value == value) || value == 0.0f) value = 0.0f;  // NaN and -0 sort as 0
    const auto bits = std::bit_cast<std::uint32_t>(value);
    return (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
}

// Draw order: ascending sorting_order, then sort_y, then submission order.
// Sorting keys gives the stable order without moving requests.
void BuildDrawOrder(const std::vector<ImageDrawRequest> &requests, std::vector<DrawOrderKey> &order) {
    order.resize(requests.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto biased = static_cast<std::uint32_t>(requests[i].sorting_order) ^ 0x80000000u;
        order[i] = {(static_cast<std::uint64_t>(biased) << 32) | SortableFloatBits(requests[i].sort_y),
                    static_cast<std::uint32_t>(i)};
    }
    std::sort(order.begin(), order.end());
}
std::size_t OrderIndex(const DrawOrderKey &key) { return key.index; }

bool CompareParticleBatchRequests(const ParticleBatchRequest &a,
                                  const ParticleBatchRequest &b) {
    return a.sorting_order < b.sorting_order;
}

bool ShouldDiscardSceneRequest(const ImageDrawRequest &request) {
    if (request.a <= 0) return true;
    if (request.scale_x == 0.0f) return true;
    if (request.scale_y == 0.0f) return true;
    return false;
}

bool ShouldDiscardUIRequest(const ImageDrawRequest &request) {
    return request.a <= 0;
}

bool TryBuildSpritesheetSourceRect(const ImageDrawRequest &request,
                                   float texture_width,
                                   float texture_height,
                                   SDL_FRect &out_source_rect) {
    const int rows = std::max(request.spritesheet_rows, 1);
    const int columns = std::max(request.spritesheet_columns, 1);
    if (rows <= 1 && columns <= 1) return false;

    const int texture_width_i = static_cast<int>(texture_width);
    const int texture_height_i = static_cast<int>(texture_height);
    if (texture_width_i <= 0 || texture_height_i <= 0) return false;

    const int cell_width = texture_width_i / columns;
    const int cell_height = texture_height_i / rows;
    if (cell_width <= 0 || cell_height <= 0) return false;

    const int row_index = std::clamp(request.sprite_row, 1, rows) - 1;
    const int column_index = std::clamp(request.sprite_column, 1, columns) - 1;
    out_source_rect.x = static_cast<float>(column_index * cell_width);
    out_source_rect.y = static_cast<float>(row_index * cell_height);
    out_source_rect.w = static_cast<float>(cell_width);
    out_source_rect.h = static_cast<float>(cell_height);
    return true;
}

bool IsPivotBoundOutsideViewport(const SDL_FRect &dst, const SDL_FPoint &pivot,
                                 float viewport_width,
                                 float viewport_height) {
    const float left = pivot.x;
    const float right = dst.w - pivot.x;
    const float top = pivot.y;
    const float bottom = dst.h - pivot.y;
    const float max_dx = std::max(left, right);
    const float max_dy = std::max(top, bottom);
    const float radius = std::sqrt(max_dx * max_dx + max_dy * max_dy);
    const float pivot_x = dst.x + pivot.x;
    const float pivot_y = dst.y + pivot.y;

    return pivot_x + radius <= 0.0f || pivot_y + radius <= 0.0f ||
           pivot_x - radius >= viewport_width ||
           pivot_y - radius >= viewport_height;
}

void DestroyCachedTextTexture(CachedTextTexture &cached) {
    if (cached.text != nullptr) TTF_DestroyText(cached.text);
    cached.text = nullptr;
}

SDL_Texture *CreateDefaultParticleTexture(SDL_Renderer *renderer) {
    if (renderer == nullptr) return nullptr;

    // Create an SDL_Surface (a cpu-side texture) with no special flags, 8 width, 8 height, 32 bits of color depth (RGBA) and no masking.
    SDL_Surface *surface = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_RGBA8888);
    if (surface == nullptr) return nullptr;

    // Ensure color set to white (255, 255, 255, 255)
    const Uint32 white_color = SDL_MapSurfaceRGBA(surface, 255, 255, 255, 255);
    SDL_FillSurfaceRect(surface, nullptr, white_color);

    // Create a gpu-side texture from the cpu-side surface now that we're done editing it.
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);

    // Clean up the surface and cache this default texture for future use (we'll probably spawn many particles with it).
    SDL_DestroySurface(surface);
    if (texture != nullptr) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }
    return texture;
}

SDL_Texture *CreateMissingTexture(SDL_Renderer *renderer) {
    if (renderer == nullptr) return nullptr;

    SDL_Surface *surface = SDL_CreateSurface(32, 32, SDL_PIXELFORMAT_RGBA8888);
    if (surface == nullptr) return nullptr;

    const Uint32 magenta = SDL_MapSurfaceRGBA(surface, 255, 0, 255, 255);
    const Uint32 black = SDL_MapSurfaceRGBA(surface, 0, 0, 0, 255);
    for (int y = 0; y < 32; y += 8) {
        for (int x = 0; x < 32; x += 8) {
            SDL_Rect rect = {x, y, 8, 8};
            const bool bright_square = ((x / 8) + (y / 8)) % 2 == 0;
            SDL_FillSurfaceRect(surface, &rect, bright_square ? magenta : black);
        }
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture != nullptr) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }
    return texture;
}

} // namespace

RenderQueueState &Renderer::Queues() {
    return World::Current().State<RenderQueueState>();
}

void Renderer::PruneTextCache() {
    const Uint32 now = SDL_GetTicks();
    for (auto it = text_cache.begin(); it != text_cache.end();) {
        CachedTextTexture &cached = it->second;
        if (cached.last_used_timestamp != 0 && now - cached.last_used_timestamp > kTextCacheUnusedLifetimeMs) {
            DestroyCachedTextTexture(cached);
            it = text_cache.erase(it);
            continue;
        }
        ++it;
    }
    if (text_cache.size() <= kMaxTextCacheEntries) return;
    // Evict the least recently used entries in one pass.
    std::vector<std::pair<Uint32, std::string>> by_age;
    by_age.reserve(text_cache.size());
    for (const auto &[key, cached] : text_cache) by_age.emplace_back(cached.last_used_timestamp, key);
    const std::size_t excess = text_cache.size() - kMaxTextCacheEntries;
    std::nth_element(by_age.begin(), by_age.begin() + excess, by_age.end());
    for (std::size_t i = 0; i < excess; ++i) {
        const auto found = text_cache.find(by_age[i].second);
        DestroyCachedTextTexture(found->second);
        text_cache.erase(found);
    }
}

void Renderer::ResetTextEngine() {
    for (auto &entry : text_cache) DestroyCachedTextTexture(entry.second);
    text_cache.clear();
    if (text_engine != nullptr) TTF_DestroyRendererTextEngine(text_engine);
    text_engine = nullptr;
    text_engine_renderer = nullptr;
}

// Initialize shared rendering backends and per-session caches.
void Renderer::Init() {
    if (initialized) return;
    if (!TTF_Init()) {
        LogRendererWarningOnce("ttf:init",
                               std::string("text rendering disabled: ") +
                                   SDL_GetError());
        return;
    }
    initialized = true;
}

// Release cached fonts, textures, and queued draw state for the session.
void Renderer::Shutdown() {
    // Text objects reference fonts and the engine's atlas; release them first.
    ResetTextEngine();
    for (auto &font_family : font_cache) {
        for (auto &entry : font_family.second) {
            if (entry.second != nullptr) {
                TTF_CloseFont(entry.second);
            }
        }
    }
    font_cache.clear();
    Queues().text_draw_requests.clear();

    ClearCache();

    if (initialized) {
        TTF_Quit();
        initialized = false;
    }
    g_renderer_warning_keys.clear();
    g_renderer_warning_count = 0;
    g_renderer_last_warning.clear();
}

// Resolve and cache one font family/size pair for later text rendering.
TTF_Font *Renderer::GetFont(const std::string &font_name, int size) {
    if (font_name.empty()) return nullptr;
    if (!initialized) return nullptr;
    auto family_it = font_cache.find(font_name);
    if (family_it != font_cache.end()) {
        auto size_it = family_it->second.find(size);
        if (size_it != family_it->second.end()) return size_it->second;
    }

    std::string path = ResolveFontPath(font_name);
    if (path.empty()) {
        LogRendererWarningOnce("font:missing:" + font_name,
                               "font [" + font_name +
                                   "] missing; using editor system font");
        path = ResolveSystemFontPath();
    }
    if (path.empty()) {
        LogRendererWarningOnce("font:missing-system",
                               "system font missing; skipping text draw");
        return nullptr;
    }

    TTF_Font *font = TTF_OpenFont(path.c_str(), size);
    if (font == nullptr) {
        LogRendererWarningOnce("font:open:" + font_name,
                               "font [" + font_name +
                                   "] could not be opened; skipping text draw");
        return nullptr;
    }
    font_cache[font_name][size] = font;
    return font;
}

// resource loading and draw queue entry points
Renderer::CachedImageTexture *Renderer::LoadTextureResource(
    const std::string &image_name, SDL_Renderer *renderer) {
    if (image_name.empty() || renderer == nullptr) return nullptr;

    // Called for every draw: compare the root's native string in place.
    const auto &resources_root = ResourcePath::MutableResourcesRootPath().native();
    if (image_cache_resources_root.empty()) {
        image_cache_resources_root = resources_root;
    } else if (image_cache_resources_root != resources_root) {
        ClearImageTextureCache();
        image_cache_resources_root = resources_root;
    }

    auto it = image_cache.find(image_name);
    if (it != image_cache.end()) {
        return &it->second;
    }

    if (image_name == kDefaultParticleTextureName) {
        SDL_Texture *texture = CreateDefaultParticleTexture(renderer);
        if (texture == nullptr) {
            LogRendererWarningOnce("image:default-particle",
                                   "failed to create default particle texture");
            return nullptr;
        }
        CachedImageTexture cached;
        cached.texture = texture;
        cached.width = 8.0f;
        cached.height = 8.0f;
        auto inserted = image_cache.emplace(image_name, cached);
        return &inserted.first->second;
    }

    std::string image_path = ResolveImagePath(image_name);
    if (image_path.empty()) {
        LogRendererWarningOnce("image:missing:" + image_name,
                               "image [" + image_name +
                                   "] missing; using placeholder");
        SDL_Texture *placeholder = CreateMissingTexture(renderer);
        if (placeholder == nullptr) {
            return nullptr;
        }
        CachedImageTexture cached;
        cached.texture = placeholder;
        cached.width = 32.0f;
        cached.height = 32.0f;
        cached.missing = true;
        auto inserted = image_cache.emplace(image_name, cached);
        return &inserted.first->second;
    }

    SDL_Texture *texture = IMG_LoadTexture(renderer, image_path.c_str());
    if (texture == nullptr) {
        LogRendererWarningOnce("image:load:" + image_name,
                               "image [" + image_name +
                                   "] failed to load; using placeholder");
        texture = CreateMissingTexture(renderer);
    }

    if (texture == nullptr) {
        return nullptr;
    }

    // SDL3 defaults to linear filtering. World/UI image assets retain the
    // engine's pixel-art sampling contract; text keeps its separate filtering.
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    float texture_width = 0;
    float texture_height = 0;
    SDL_GetTextureSize(texture, &texture_width, &texture_height);

    CachedImageTexture cached;
    cached.texture = texture;
    cached.width = static_cast<float>(texture_width);
    cached.height = static_cast<float>(texture_height);
    cached.source = image_path;
    std::error_code time_error;
    cached.modified = std::filesystem::last_write_time(cached.source, time_error);
    auto inserted = image_cache.emplace(image_name, cached);
    return &inserted.first->second;
}

int Renderer::ReloadChangedImages() {
    int dropped = 0;
    for (auto it = image_cache.begin(); it != image_cache.end();) {
        const CachedImageTexture &cached = it->second;
        if (cached.source.empty() && !cached.missing) { ++it; continue; }
        if (!cached.missing) {
            // A placeholder for a missing image is always retried.
            std::error_code error;
            const auto modified = std::filesystem::last_write_time(cached.source, error);
            if (!error && modified == cached.modified) { ++it; continue; }
        }
        if (cached.texture != nullptr) SDL_DestroyTexture(cached.texture);
        it = image_cache.erase(it);
        ++dropped;
    }
    if (dropped > 0) image_cache_by_id.clear();
    return dropped;
}

namespace {
struct ImageNames {
    std::mutex mutex;
    std::unordered_map<std::string, std::uint32_t> ids;
    std::deque<std::string> names; // id - 1 -> name; deque keeps references stable
};
ImageNames &InternedImages() {
    static ImageNames images;
    return images;
}
} // namespace

std::uint32_t Renderer::InternImage(const std::string &image_name) {
    auto &images = InternedImages();
    const std::lock_guard lock(images.mutex);
    const auto [found, inserted] = images.ids.try_emplace(image_name, 0);
    if (inserted) {
        images.names.push_back(image_name);
        found->second = static_cast<std::uint32_t>(images.names.size());
    }
    return found->second;
}

void Renderer::SyncImageCacheRoot() {
    const auto &resources_root = ResourcePath::MutableResourcesRootPath().native();
    if (image_cache_resources_root.empty()) {
        image_cache_resources_root = resources_root;
    } else if (image_cache_resources_root != resources_root) {
        ClearImageTextureCache();
        image_cache_resources_root = resources_root;
    }
}

Renderer::CachedImageTexture *Renderer::LoadTextureResource(const ImageDrawRequest &request,
                                                            SDL_Renderer *renderer) {
    if (request.image_id == 0) return LoadTextureResource(request.image_name, renderer);
    const std::size_t index = request.image_id - 1;
    if (index < image_cache_by_id.size() && image_cache_by_id[index] != nullptr) return image_cache_by_id[index];
    std::string name;
    {
        auto &images = InternedImages();
        const std::lock_guard lock(images.mutex);
        if (index >= images.names.size()) return nullptr;
        name = images.names[index];
    }
    CachedImageTexture *cached = LoadTextureResource(name, renderer);
    if (cached != nullptr) {
        if (image_cache_by_id.size() <= index) image_cache_by_id.resize(index + 1, nullptr);
        image_cache_by_id[index] = cached;
    }
    return cached;
}

SDL_Texture *Renderer::LoadTexture(const std::string &image_name,
                                   SDL_Renderer *renderer) {
    CachedImageTexture *cached = LoadTextureResource(image_name, renderer);
    return cached == nullptr ? nullptr : cached->texture;
}

SDL_Texture *Renderer::LoadTexture(const std::string &image_name,
                                   SDL_Renderer *renderer, float *width,
                                   float *height) {
    CachedImageTexture *cached = LoadTextureResource(image_name, renderer);
    if (cached == nullptr) return nullptr;
    if (width != nullptr) *width = cached->width;
    if (height != nullptr) *height = cached->height;
    return cached->texture;
}

// resource loading and draw queue entry points
void Renderer::QueueSceneImageDraw(const ImageDrawRequest &request) {
    if (ShouldDiscardSceneRequest(request)) return;
    Queues().scene_draw_requests.push_back(request);
}

// resource loading and draw queue entry points
void Renderer::QueueSceneParticleBatch(const ParticleBatchRequest &request) {
    if (request.emitter_id < 0) return;
    Queues().scene_particle_batches.push_back(request);
}

// resource loading and draw queue entry points
void Renderer::QueueUIImageDraw(const ImageDrawRequest &request) {
    if (ShouldDiscardUIRequest(request)) return;
    Queues().ui_draw_requests.push_back(request);
}

// resource loading and draw queue entry points
void Renderer::QueuePixelDraw(const PixelDrawRequest &request) {
    Queues().pixel_draw_requests.push_back(request);
}

// text draw helpers
void Renderer::DrawText(const std::string &text_content,
                        const std::string &font_name, int font_size,
                        SDL_Color font_color, int x, int y) {
    if (font_name.empty()) return;
    Queues().text_draw_requests.push_back( {text_content, font_name, font_size, font_color, x, y});
}

void Renderer::DiscardFrameRequests() {
    Queues().scene_draw_requests.clear();
    Queues().scene_particle_batches.clear();
    Queues().ui_draw_requests.clear();
    Queues().pixel_draw_requests.clear();
    Queues().text_draw_requests.clear();
}

// Render one frame by flushing queued scene images, particles, text, and pixels.
void Renderer::RenderFrame(SDL_Renderer *renderer, float camera_x,
                           float camera_y, float zoom_factor,
                           int camera_width, int camera_height,
                           float output_scale_x, float output_scale_y) {
    POCKET_ZONE("Renderer::RenderFrame");
    const float safe_output_scale_x = SafeOutputScale(output_scale_x);
    const float safe_output_scale_y = SafeOutputScale(output_scale_y);
    RenderAndClearAllImages(renderer, camera_x, camera_y, zoom_factor,
                            camera_width, camera_height, safe_output_scale_x,
                            safe_output_scale_y);
    RenderAndClearAllText(renderer, safe_output_scale_x,
                          safe_output_scale_y);
    RenderAndClearAllPixels(renderer);
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    PruneTextCache();
}

// Flush queued world-space image and particle draw requests for this frame.
void Renderer::RenderAndClearAllImages(SDL_Renderer *renderer, float camera_x,
                                       float camera_y, float zoom_factor,
                                       int camera_width, int camera_height,
                                       float output_scale_x,
                                       float output_scale_y) {
    if (renderer == nullptr) {
        Queues().scene_draw_requests.clear();
        Queues().scene_particle_batches.clear();
        Queues().ui_draw_requests.clear();
        return;
    }

    SyncImageCacheRoot();
    Queues().scene_draw_requests.erase(
        std::remove_if(Queues().scene_draw_requests.begin(), Queues().scene_draw_requests.end(),
                       ShouldDiscardSceneRequest),
        Queues().scene_draw_requests.end());
    Queues().ui_draw_requests.erase(
        std::remove_if(Queues().ui_draw_requests.begin(), Queues().ui_draw_requests.end(),
                       ShouldDiscardUIRequest),
        Queues().ui_draw_requests.end());

    RenderQueueState &queues = Queues();
    BuildDrawOrder(queues.scene_draw_requests, queues.scene_draw_order);
    if (Queues().scene_particle_batches.size() > 1) {
        std::stable_sort(Queues().scene_particle_batches.begin(),
                         Queues().scene_particle_batches.end(),
                         CompareParticleBatchRequests);
    }
    BuildDrawOrder(queues.ui_draw_requests, queues.ui_draw_order);

    const float safe_zoom = (zoom_factor > 0.0f) ? zoom_factor : 1.0f;
    const float viewport_width = static_cast<float>(camera_width) * (1.0f / safe_zoom);
    const float viewport_height = static_cast<float>(camera_height) * (1.0f / safe_zoom);

    const auto render_scene_request = [&](const ImageDrawRequest &request) {
        float texture_w = 0.0f;
        float texture_h = 0.0f;
        const CachedImageTexture *cached = LoadTextureResource(request, renderer);
        SDL_Texture *texture = cached == nullptr ? nullptr : cached->texture;
        if (cached != nullptr) { texture_w = cached->width; texture_h = cached->height; }
        if (texture == nullptr) return;

        SDL_FRect source_rect;
        SDL_FRect *source_rect_ptr = nullptr;
        float draw_width = texture_w;
        float draw_height = texture_h;
        if (TryBuildSpritesheetSourceRect(request, texture_w, texture_h,
                                          source_rect)) {
            source_rect_ptr = &source_rect;
            draw_width = source_rect.w;
            draw_height = source_rect.h;
        }

        SDL_FlipMode flip = SDL_FLIP_NONE;
        if (request.scale_x < 0.0f) {
            flip = static_cast<SDL_FlipMode>(flip | SDL_FLIP_HORIZONTAL);
        }
        if (request.scale_y < 0.0f) {
            flip = static_cast<SDL_FlipMode>(flip | SDL_FLIP_VERTICAL);
        }

        const float x_scale = glm::abs(request.scale_x);
        const float y_scale = glm::abs(request.scale_y);

        SDL_FRect dst;
        dst.w = draw_width * x_scale;
        dst.h = draw_height * y_scale;

        SDL_FPoint pivot = {request.pivot_x * dst.w, request.pivot_y * dst.h};

        const float final_x = request.x - camera_x;
        const float final_y = request.y - camera_y;
        dst.x = final_x * kPixelsPerUnit +
                static_cast<float>(camera_width) * 0.5f * (1.0f / safe_zoom) -
                pivot.x;
        dst.y = final_y * kPixelsPerUnit +
                static_cast<float>(camera_height) * 0.5f * (1.0f / safe_zoom) -
                pivot.y;

        if (IsPivotBoundOutsideViewport(dst, pivot, viewport_width,
                                        viewport_height)) {
            return;
        }
        if (request.rotation_degrees == 0.0f) {
            // Snap both edges to output pixels so abutting draws (tiles) share
            // an edge instead of rounding apart into one-pixel seams.
            const float to_x = safe_zoom * output_scale_x;
            const float to_y = safe_zoom * output_scale_y;
            const float left = std::round(dst.x * to_x), right = std::round((dst.x + dst.w) * to_x);
            const float top = std::round(dst.y * to_y), bottom = std::round((dst.y + dst.h) * to_y);
            dst = {left / to_x, top / to_y, (right - left) / to_x, (bottom - top) / to_y};
        }

        SDL_SetTextureColorMod(texture, static_cast<Uint8>(ClampByte(request.r)),
                               static_cast<Uint8>(ClampByte(request.g)),
                               static_cast<Uint8>(ClampByte(request.b)));
        SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(ClampByte(request.a)));

        SDLRenderHelper::SDL_RenderTextureRotated(
            Actor::kInvalidUID, "", renderer, texture, source_rect_ptr, &dst,
            request.rotation_degrees,
            &pivot, flip);

        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(texture, 255);
    };

    SDL_SetRenderScale(renderer, safe_zoom * output_scale_x,
                      safe_zoom * output_scale_y);
    size_t scene_request_index = 0;
    size_t particle_batch_index = 0;
    const auto scene_request = [&](std::size_t position) -> const ImageDrawRequest & {
        return queues.scene_draw_requests[OrderIndex(queues.scene_draw_order[position])];
    };
    while (scene_request_index < queues.scene_draw_requests.size() ||
           particle_batch_index < queues.scene_particle_batches.size()) {
        const bool should_render_scene_request =
            (particle_batch_index >= queues.scene_particle_batches.size()) ||
            (scene_request_index < queues.scene_draw_requests.size() &&
             scene_request(scene_request_index).sorting_order <=
                 queues.scene_particle_batches[particle_batch_index].sorting_order);

        if (should_render_scene_request) {
            render_scene_request(scene_request(scene_request_index));
            ++scene_request_index;
            continue;
        }

        ParticleManager::RenderEmitterBatch(
            Queues().scene_particle_batches[particle_batch_index].emitter_id, renderer,
            camera_x, camera_y, zoom_factor, camera_width, camera_height);
        ++particle_batch_index;
    }
    SDL_SetRenderScale(renderer, output_scale_x, output_scale_y);

    for (const DrawOrderKey &key : queues.ui_draw_order) {
        const ImageDrawRequest &request = queues.ui_draw_requests[OrderIndex(key)];
        float texture_w = 0.0f;
        float texture_h = 0.0f;
        const CachedImageTexture *cached = LoadTextureResource(request, renderer);
        SDL_Texture *texture = cached == nullptr ? nullptr : cached->texture;
        if (cached != nullptr) { texture_w = cached->width; texture_h = cached->height; }
        if (texture == nullptr) continue;

        SDL_FRect source_rect;
        SDL_FRect *source_rect_ptr = nullptr;
        float draw_width = texture_w;
        float draw_height = texture_h;
        if (TryBuildSpritesheetSourceRect(request, texture_w, texture_h,
                                          source_rect)) {
            source_rect_ptr = &source_rect;
            draw_width = source_rect.w;
            draw_height = source_rect.h;
        }

        SDL_FRect dst = {request.x, request.y, draw_width, draw_height};
        SDL_SetTextureColorMod(texture, static_cast<Uint8>(ClampByte(request.r)),
                               static_cast<Uint8>(ClampByte(request.g)),
                               static_cast<Uint8>(ClampByte(request.b)));
        SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(ClampByte(request.a)));

        SDLRenderHelper::SDL_RenderTextureRotated(Actor::kInvalidUID, "", renderer,
                                          texture, source_rect_ptr,
                                          &dst, 0.0f, nullptr, SDL_FLIP_NONE);

        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(texture, 255);
    }

    Queues().scene_draw_requests.clear();
    Queues().scene_particle_batches.clear();
    Queues().ui_draw_requests.clear();
}

// Flush queued text draw requests and reuse cached text textures when possible.
void Renderer::RenderAndClearAllText(SDL_Renderer *renderer,
                                     float output_scale_x,
                                     float output_scale_y) {
    const float safe_output_scale_x = SafeOutputScale(output_scale_x);
    const float safe_output_scale_y = SafeOutputScale(output_scale_y);
    const float raster_scale =
        std::max(safe_output_scale_x, safe_output_scale_y);
    SDL_SetRenderScale(renderer, safe_output_scale_x, safe_output_scale_y);

    if (!Queues().text_draw_requests.empty() && text_engine_renderer != renderer) {
        ResetTextEngine();
        text_engine = TTF_CreateRendererTextEngine(renderer);
        if (text_engine == nullptr)
            LogRendererWarningOnce("ttf:engine", std::string("text engine unavailable: ") + SDL_GetError());
        else text_engine_renderer = renderer;
    }
    // Glyph quads are laid out in output pixels at the rasterized size.
    SDL_SetRenderScale(renderer, 1.0f, 1.0f);
    for (const TextDrawRequest &req : Queues().text_draw_requests) {
        if (req.font_size <= 0 || req.text.empty() || text_engine == nullptr) continue;

        const int raster_font_size =
            std::max(1, static_cast<int>(std::lround(
                            static_cast<float>(req.font_size) *
                            raster_scale)));
        std::string cache_key;
        cache_key.reserve(req.font_name.size() + req.text.size() + 16);
        cache_key.append(req.font_name).push_back('|');
        cache_key.append(std::to_string(raster_font_size)).push_back('|');
        cache_key.append(req.text);

        auto cache_it = text_cache.find(cache_key);
        if (cache_it == text_cache.end()) {
            TTF_Font *font = GetFont(req.font_name, raster_font_size);
            if (font == nullptr) continue;
            TTF_Text *text = TTF_CreateText(text_engine, font, req.text.c_str(), req.text.size());
            if (text == nullptr) continue;
            cache_it = text_cache.emplace(std::move(cache_key), CachedTextTexture{text, 0}).first;
        }
        cache_it->second.last_used_timestamp = SDL_GetTicks();
        TTF_Text *text = cache_it->second.text;
        TTF_SetTextColor(text, req.color.r, req.color.g, req.color.b, req.color.a);
        TTF_DrawRendererText(text, static_cast<float>(req.x) * safe_output_scale_x,
                             static_cast<float>(req.y) * safe_output_scale_y);
    }
    SDL_SetRenderScale(renderer, safe_output_scale_x, safe_output_scale_y);

    Queues().text_draw_requests.clear();
}

// Flush queued pixel draw requests for debug/overlay rendering.
void Renderer::RenderAndClearAllPixels(SDL_Renderer *renderer) {
    if (renderer == nullptr) {
        Queues().pixel_draw_requests.clear();
        return;
    }
    if (Queues().pixel_draw_requests.empty()) return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (const PixelDrawRequest &request : Queues().pixel_draw_requests) {
        SDL_SetRenderDrawColor(renderer, ClampByteU8(request.r),
                               ClampByteU8(request.g), ClampByteU8(request.b),
                               ClampByteU8(request.a));
        SDL_RenderPoint(renderer, request.x, request.y);
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    Queues().pixel_draw_requests.clear();
}

// Clear resource caches and per-frame draw queues.
void Renderer::ClearImageTextureCache() {
    image_cache_by_id.clear();
    for (auto &entry : image_cache) {
        if (entry.second.texture != nullptr) {
            SDL_DestroyTexture(entry.second.texture);
        }
    }
    image_cache.clear();
}

void Renderer::ClearCache() {
    // The text engine's atlas textures belong to its renderer too.
    ResetTextEngine();
    ClearImageTextureCache();
    image_cache_resources_root.clear();
    Queues().scene_draw_requests.clear();
    Queues().scene_particle_batches.clear();
    Queues().ui_draw_requests.clear();
    Queues().pixel_draw_requests.clear();
}

int Renderer::GetResourceWarningCount() {
    return g_renderer_warning_count;
}

const std::string &Renderer::GetLastResourceWarning() {
    return g_renderer_last_warning;
}

Engine::AssetReload Engine::ReloadChangedAssets() {
    WorldScope world_scope(world_);
    AssetReload result;
    result.spritesheet_metadata = SpritesheetConfig::ReloadIfChanged();
    result.images = Renderer::ReloadChangedImages();
    if (result.images > 0 || result.spritesheet_metadata)
        Log::Info("render", "hot reload: " + std::to_string(result.images) + " image(s)" +
                                (result.spritesheet_metadata ? ", spritesheet metadata" : ""));
    return result;
}

void Engine::render() {
    POCKET_ZONE("Engine::render");
    if (asset_hot_reload_ && ++asset_hot_reload_frames_ % 30 == 0) ReloadChangedAssets();
    // Rendering only fills the backbuffer. Presenting is owned by the outer
    // loop so runtime-only and editor-driven execution share one path.
    // 现在: render 只填充 backbuffer, PresentFrame 负责交换前后缓冲. 
    // 这样 runtime-only 和 editor-driven 执行就共用一套渲染流程了.
    if (render_runtime_to_texture_) {
        ensureRuntimeRenderTarget();
        SDL_SetRenderTarget(renderer, runtime_render_target_);
    } else {
        SDL_SetRenderTarget(renderer, nullptr);
    }
    SDL_SetRenderViewport(renderer, nullptr);
    clearFrame();

    // Built-in render components contribute draw requests during the render
    // phase so frozen editor frames still show authored scene content.
    ComponentManager::ResolveTransformHierarchy();
    ComponentManager::QueueBuiltinRenderers();
    ParticleManager::QueueRenderBatches();

    const float zoom_factor = std::clamp(runtime_zoom_factor, kMinZoomFactor, kMaxZoomFactor);
    const int render_width = std::max(1, config_.window_width);
    const int render_height = std::max(1, config_.window_height);
    int output_width = runtime_render_target_width_;
    int output_height = runtime_render_target_height_;
    if (!render_runtime_to_texture_ || runtime_render_target_ == nullptr) {
        output_width = render_width;
        output_height = render_height;
        SDL_GetRenderOutputSize(renderer, &output_width, &output_height);
    }
    const float output_scale_x =
        static_cast<float>(std::max(1, output_width)) /
        static_cast<float>(render_width);
    const float output_scale_y =
        static_cast<float>(std::max(1, output_height)) /
        static_cast<float>(render_height);
    const float half_world_width = render_width / (200.0f * zoom_factor);
    const float half_world_height = render_height / (200.0f * zoom_factor);
    ComponentManager::QueueTilemaps(camera_position.x - half_world_width,
                                    camera_position.y - half_world_height,
                                    camera_position.x + half_world_width,
                                    camera_position.y + half_world_height);
    Renderer::RenderFrame(renderer, camera_position.x, camera_position.y,
                          zoom_factor, render_width, render_height,
                          output_scale_x, output_scale_y);
    RecordRuntimeRenderFrame();

    if (render_runtime_to_texture_) {
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_SetRenderViewport(renderer, nullptr);
        clearEditorHostFrame();
    }
}

SDL_Surface *Engine::RenderOffscreen(int width, int height) {
    WorldScope world_scope(world_);
    if (window != nullptr || borrows_presenter_ || !runtime_initialized_) {
        SDL_SetError("offscreen rendering requires an initialized headless engine");
        return nullptr;
    }
    if (width < 1 || height < 1 || width > 4096 || height > 4096) {
        SDL_SetError("offscreen size must be within 1..4096");
        return nullptr;
    }
    if (offscreen_surface_ == nullptr || offscreen_surface_->w != width || offscreen_surface_->h != height) {
        // Cached textures belong to the renderer they were created with.
        if (renderer != nullptr) {
            Renderer::ClearCache();
            SDL_DestroyRenderer(renderer);
            renderer = nullptr;
        }
        SDL_DestroySurface(offscreen_surface_);
        offscreen_surface_ = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
        if (offscreen_surface_ == nullptr) return nullptr;
        renderer = SDL_CreateSoftwareRenderer(offscreen_surface_);
        if (renderer == nullptr) {
            SDL_DestroySurface(offscreen_surface_);
            offscreen_surface_ = nullptr;
            return nullptr;
        }
        Renderer::Init();
    }
    // render() consumes the queues; the next tick must see the same requests
    // whether or not anyone looked.
    auto &queues = world_.State<RenderQueueState>();
    const RenderQueueState saved = queues;
    render();
    SDL_Surface *pixels = SDL_RenderReadPixels(renderer, nullptr);
    queues = saved;
    if (pixels == nullptr) return nullptr;
    SDL_Surface *copy = SDL_ConvertSurface(pixels, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(pixels);
    return copy;
}

void Engine::clearFrame() {
    SDL_SetRenderDrawColor(renderer, ClampByteU8(config_.clear_color_r),
                           ClampByteU8(config_.clear_color_g),
                           ClampByteU8(config_.clear_color_b), 255);
    SDL_RenderClear(renderer);
}

void Engine::clearEditorHostFrame() {
    // The editor chrome owns the host window. Once the runtime has been drawn
    // into its offscreen texture, reset the backbuffer to a neutral color so
    // Dear ImGui can compose the dockspace and panels on top.
    SDL_SetRenderDrawColor(renderer, 18, 22, 30, 255);
    SDL_RenderClear(renderer);
}
