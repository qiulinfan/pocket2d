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
#include <cmath>
#include <cstring>
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

struct DynamicImage {
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels;
    Renderer::DynamicImageStyle style;
    std::uint64_t revision = 0;
};
struct DynamicImages {
    std::mutex mutex;
    std::unordered_map<std::string, DynamicImage> images;
    std::uint64_t next_revision = 1;
};
DynamicImages &Dynamics() {
    static DynamicImages images;
    return images;
}
bool IsDynamicName(const std::string &name) { return name.starts_with(Renderer::kDynamicImagePrefix); }

struct ImageSizeCache {
    std::mutex mutex;
    std::unordered_map<std::string, std::pair<int, int>> sizes;
};
ImageSizeCache &ImageSizes() {
    static ImageSizeCache cache;
    return cache;
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

// Applies a request's blend to its texture and returns the texture's own mode
// (normal or a light map's multiply) to restore after the draw.
SDL_BlendMode BeginRequestBlend(SDL_Texture *texture, ImageBlend blend) {
    SDL_BlendMode own = SDL_BLENDMODE_BLEND;
    SDL_GetTextureBlendMode(texture, &own);
    if (blend == ImageBlend::Add) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD);
    else if (blend == ImageBlend::Multiply) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_MUL);
    return own;
}

// SDL's software renderer rotates a texture drawn with SDL_BLENDMODE_MUL onto a
// white, alpha-0 background that its MUL blit does not leave out, so the corners
// of the rotated rectangle double what is under them. A rotated multiplied draw
// goes through geometry there instead (two triangles, the same pixels elsewhere).
bool RendersRotatedMultiplyAsGeometry(SDL_Renderer *renderer, const ImageDrawRequest &request) {
    if (request.blend != ImageBlend::Multiply || request.rotation_degrees == 0.0f) return false;
    const char *name = SDL_GetRendererName(renderer);
    return name != nullptr && std::strcmp(name, SDL_SOFTWARE_RENDERER) == 0;
}

void RenderRotatedGeometry(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_FRect *source,
                           float texture_w, float texture_h, const SDL_FRect &dst, double degrees,
                           SDL_FPoint pivot, SDL_FlipMode flip, const ImageDrawRequest &request) {
    const SDL_FRect src = source ? *source : SDL_FRect{0.0f, 0.0f, texture_w, texture_h};
    float u0 = src.x / texture_w, u1 = (src.x + src.w) / texture_w;
    float v0 = src.y / texture_h, v1 = (src.y + src.h) / texture_h;
    if (flip & SDL_FLIP_HORIZONTAL) std::swap(u0, u1);
    if (flip & SDL_FLIP_VERTICAL) std::swap(v0, v1);
    const double radians = degrees * 3.14159265358979323846 / 180.0;
    const float c = static_cast<float>(std::cos(radians)), sn = static_cast<float>(std::sin(radians));
    const float cx = dst.x + pivot.x, cy = dst.y + pivot.y;
    const SDL_FColor color = {static_cast<float>(ClampByte(request.r)) / 255.0f, static_cast<float>(ClampByte(request.g)) / 255.0f,
                              static_cast<float>(ClampByte(request.b)) / 255.0f, static_cast<float>(ClampByte(request.a)) / 255.0f};
    SDL_Vertex v[4];
    const float xs[4] = {dst.x, dst.x + dst.w, dst.x + dst.w, dst.x};
    const float ys[4] = {dst.y, dst.y, dst.y + dst.h, dst.y + dst.h};
    const float us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
    for (int i = 0; i < 4; ++i) {
        const float dx = xs[i] - cx, dy = ys[i] - cy;
        v[i].position = {cx + dx * c - dy * sn, cy + dx * sn + dy * c};
        v[i].color = color;
        v[i].tex_coord = {us[i], vs[i]};
    }
    const int indices[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, texture, v, 4, indices, 6);
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
    if (IsDynamicName(image_name)) {
        auto &dynamics = Dynamics();
        const std::lock_guard lock(dynamics.mutex);
        const auto found = dynamics.images.find(image_name);
        if (found == dynamics.images.end()) return nullptr;
        const DynamicImage &image = found->second;
        if (it != image_cache.end() && it->second.dynamic_revision == image.revision) return &it->second;
        CachedImageTexture &cached = image_cache[image_name];
        if (cached.texture != nullptr && (static_cast<int>(cached.width) != image.width ||
                                          static_cast<int>(cached.height) != image.height)) {
            SDL_DestroyTexture(cached.texture);
            cached.texture = nullptr;
        }
        if (cached.texture == nullptr) {
            cached.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                               image.width, image.height);
            if (cached.texture == nullptr) { image_cache.erase(image_name); return nullptr; }
        }
        SDL_SetTextureBlendMode(cached.texture, image.style.multiply ? SDL_BLENDMODE_MOD : SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(cached.texture, image.style.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        SDL_UpdateTexture(cached.texture, nullptr, image.pixels.data(), image.width * 4);
        cached.width = static_cast<float>(image.width);
        cached.height = static_cast<float>(image.height);
        cached.dynamic = true;
        cached.dynamic_revision = image.revision;
        return &cached;
    }
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
    {
        const std::lock_guard lock(ImageSizes().mutex);
        ImageSizes().sizes.clear();
    }
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
    if (index < image_cache_by_id.size() && image_cache_by_id[index] != nullptr &&
        !image_cache_by_id[index]->dynamic) return image_cache_by_id[index];
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

void Renderer::SetDynamicImage(const std::string &name, int width, int height, std::vector<std::uint8_t> rgba) {
    SetDynamicImage(name, width, height, std::move(rgba), DynamicImageStyle{});
}

void Renderer::SetDynamicImage(const std::string &name, int width, int height, std::vector<std::uint8_t> rgba,
                               DynamicImageStyle style) {
    if (!IsDynamicName(name)) throw std::invalid_argument("dynamic image names must start with \"dynamic:\"");
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        rgba.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)
        throw std::invalid_argument("dynamic image needs 1..4096 px sides and width*height RGBA bytes");
    auto &dynamics = Dynamics();
    const std::lock_guard lock(dynamics.mutex);
    auto &image = dynamics.images[name];
    image.width = width;
    image.height = height;
    image.pixels = std::move(rgba);
    image.style = style;
    image.revision = dynamics.next_revision++;
}

bool Renderer::GetImageSize(const std::string &image_name, int &width, int &height) {
    if (IsDynamicName(image_name)) {
        auto &dynamics = Dynamics();
        const std::lock_guard lock(dynamics.mutex);
        const auto found = dynamics.images.find(image_name);
        if (found == dynamics.images.end()) return false;
        width = found->second.width;
        height = found->second.height;
        return true;
    }
    auto &cache = ImageSizes();
    {
        const std::lock_guard lock(cache.mutex);
        const auto found = cache.sizes.find(image_name);
        if (found != cache.sizes.end()) {
            width = found->second.first;
            height = found->second.second;
            return true;
        }
    }
    const std::string path = ResolveImagePath(image_name);
    if (path.empty()) return false;
    SDL_Surface *surface = IMG_Load(path.c_str());
    if (surface == nullptr) return false;
    width = surface->w;
    height = surface->h;
    SDL_DestroySurface(surface);
    const std::lock_guard lock(cache.mutex);
    cache.sizes[image_name] = {width, height};
    return true;
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

void Renderer::HoldFrameRequests(bool match_previous) {
    RenderQueueState &q = Queues();
    q.previous_scene_draw_requests.swap(q.held_scene_draw_requests);
    q.held_scene_draw_requests.swap(q.scene_draw_requests);
    q.held_scene_particle_batches.swap(q.scene_particle_batches);
    q.held_ui_draw_requests.swap(q.ui_draw_requests);
    q.held_pixel_draw_requests.swap(q.pixel_draw_requests);
    q.held_text_draw_requests.swap(q.text_draw_requests);
    DiscardFrameRequests();

    q.held_scene_matches.clear();
    if (!match_previous) {
        q.previous_scene_draw_requests.clear();
        return;
    }
    // Pair keyed requests across the two ticks: sorted by (key, submission
    // index), equal keys pair up in submission order.
    const auto keyed = [](const std::vector<ImageDrawRequest> &requests) {
        std::vector<std::pair<std::uint64_t, std::uint32_t>> keys;
        for (std::uint32_t i = 0; i < requests.size(); ++i)
            if (requests[i].interpolation_key != 0) keys.emplace_back(requests[i].interpolation_key, i);
        std::sort(keys.begin(), keys.end());
        return keys;
    };
    const auto previous = keyed(q.previous_scene_draw_requests);
    if (previous.empty()) return;
    const auto held = keyed(q.held_scene_draw_requests);
    if (held.empty()) return;
    q.held_scene_matches.assign(q.held_scene_draw_requests.size(), RenderQueueState::kNoMatch);
    for (std::size_t a = 0, b = 0; a < previous.size() && b < held.size();) {
        if (previous[a].first < held[b].first) ++a;
        else if (held[b].first < previous[a].first) ++b;
        else q.held_scene_matches[held[b++].second] = previous[a++].second;
    }
}

void Renderer::RequeueHeldFrameRequests(float alpha, float snap_distance) {
    RenderQueueState &q = Queues();
    const std::size_t first = q.scene_draw_requests.size();
    q.scene_draw_requests.insert(q.scene_draw_requests.end(), q.held_scene_draw_requests.begin(),
                                 q.held_scene_draw_requests.end());
    q.scene_particle_batches.insert(q.scene_particle_batches.end(), q.held_scene_particle_batches.begin(),
                                    q.held_scene_particle_batches.end());
    q.ui_draw_requests.insert(q.ui_draw_requests.end(), q.held_ui_draw_requests.begin(),
                              q.held_ui_draw_requests.end());
    q.pixel_draw_requests.insert(q.pixel_draw_requests.end(), q.held_pixel_draw_requests.begin(),
                                 q.held_pixel_draw_requests.end());
    q.text_draw_requests.insert(q.text_draw_requests.end(), q.held_text_draw_requests.begin(),
                                q.held_text_draw_requests.end());
    if (!(alpha < 1.0f) || q.held_scene_matches.empty()) return;
    alpha = std::max(alpha, 0.0f);
    for (std::size_t i = 0; i < q.held_scene_matches.size(); ++i) {
        const std::uint32_t match = q.held_scene_matches[i];
        if (match == RenderQueueState::kNoMatch) continue;
        const ImageDrawRequest &from = q.previous_scene_draw_requests[match];
        ImageDrawRequest &to = q.scene_draw_requests[first + i];
        const float dx = to.x - from.x, dy = to.y - from.y;
        if (snap_distance > 0.0f && (std::abs(dx) > snap_distance || std::abs(dy) > snap_distance)) continue;
        to.x = from.x + dx * alpha;
        to.y = from.y + dy * alpha;
        // A flip (mirrored image, whose angle changes with its facing) or a
        // turn of more than 90 degrees in one tick (a swing starting over)
        // is a cut too; otherwise turn the short way round and resize.
        if ((to.scale_x < 0.0f) != (from.scale_x < 0.0f) || (to.scale_y < 0.0f) != (from.scale_y < 0.0f))
            continue;
        const float turn = std::remainder(to.rotation_degrees - from.rotation_degrees, 360.0f);
        if (std::abs(turn) > 90.0f) continue;
        to.rotation_degrees = from.rotation_degrees + turn * alpha;
        to.scale_x = from.scale_x + (to.scale_x - from.scale_x) * alpha;
        to.scale_y = from.scale_y + (to.scale_y - from.scale_y) * alpha;
    }
}

namespace {
// splitmix64's finalizer spreads a tagged value over 64 bits; 0 means "no
// key", so it becomes 1.
std::uint64_t MixKey(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value == 0 ? 1 : value;
}
}  // namespace

std::uint64_t Renderer::InterpolationKey(double number) {
    if (number == 0.0) number = 0.0;  // -0 and 0 are one key
    return MixKey(std::bit_cast<std::uint64_t>(number) ^ 0x4e554dULL);
}

std::uint64_t Renderer::InterpolationKey(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;  // FNV-1a
    for (const char c : text) hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
    return MixKey(hash ^ 0x535452ULL);
}

std::uint64_t Renderer::ActorInterpolationKey(std::uint64_t actor_uid) {
    return MixKey(actor_uid ^ (0x41435452ULL << 32));
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
            // The renderer scales back by to_x/to_y and may truncate; a small bias
            // keeps scale-back rounding error (e.g. at zoom 0.27) on the right pixel.
            constexpr float kBias = 1e-3f;
            dst = {(left + kBias) / to_x, (top + kBias) / to_y, (right - left + kBias) / to_x,
                   (bottom - top + kBias) / to_y};
        }

        SDL_SetTextureColorMod(texture, static_cast<Uint8>(ClampByte(request.r)),
                               static_cast<Uint8>(ClampByte(request.g)),
                               static_cast<Uint8>(ClampByte(request.b)));
        SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(ClampByte(request.a)));
        const SDL_BlendMode texture_blend = BeginRequestBlend(texture, request.blend);

        if (RendersRotatedMultiplyAsGeometry(renderer, request)) {
            RenderRotatedGeometry(renderer, texture, source_rect_ptr, texture_w, texture_h, dst,
                                  request.rotation_degrees, pivot, flip, request);
        } else {
            SDLRenderHelper::SDL_RenderTextureRotated(
                Actor::kInvalidUID, "", renderer, texture, source_rect_ptr, &dst,
                request.rotation_degrees,
                &pivot, flip);
        }

        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(texture, 255);
        SDL_SetTextureBlendMode(texture, texture_blend);
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

        // UI scale (DrawUIScaled) stretches from the top-left corner; a negative
        // scale mirrors the image in place.
        SDL_FRect dst = {request.x, request.y, draw_width * std::abs(request.scale_x),
                         draw_height * std::abs(request.scale_y)};
        SDL_FlipMode ui_flip = SDL_FLIP_NONE;
        if (request.scale_x < 0.0f) ui_flip = static_cast<SDL_FlipMode>(ui_flip | SDL_FLIP_HORIZONTAL);
        if (request.scale_y < 0.0f) ui_flip = static_cast<SDL_FlipMode>(ui_flip | SDL_FLIP_VERTICAL);
        SDL_SetTextureColorMod(texture, static_cast<Uint8>(ClampByte(request.r)),
                               static_cast<Uint8>(ClampByte(request.g)),
                               static_cast<Uint8>(ClampByte(request.b)));
        SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(ClampByte(request.a)));
        const SDL_BlendMode texture_blend = BeginRequestBlend(texture, request.blend);

        SDLRenderHelper::SDL_RenderTextureRotated(Actor::kInvalidUID, "", renderer,
                                          texture, source_rect_ptr,
                                          &dst, 0.0f, nullptr, ui_flip);

        SDL_SetTextureColorMod(texture, 255, 255, 255);
        SDL_SetTextureAlphaMod(texture, 255);
        SDL_SetTextureBlendMode(texture, texture_blend);
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
    {
        const std::lock_guard lock(ImageSizes().mutex);
        ImageSizes().sizes.clear();
    }
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

void Engine::render() { render(camera_position, runtime_zoom_factor, true); }

void Engine::render(glm::vec2 camera, float zoom, bool queue_builtin_renderers) {
    POCKET_ZONE("Engine::render");
    if (asset_hot_reload_ && ++asset_hot_reload_frames_ % 30 == 0) ReloadChangedAssets();
    // Rendering only fills the backbuffer. Presenting is owned by the outer
    // loop so runtime-only and editor-driven execution share one path.
    // 现在: render 只填充 backbuffer, PresentFrame 负责交换前后缓冲. 
    // 这样 runtime-only 和 editor-driven 执行就共用一套渲染流程了.
    bool letterboxed = false;
    if (render_runtime_to_texture_) {
        ensureRuntimeRenderTarget();
        SDL_SetRenderTarget(renderer, runtime_render_target_);
    } else {
        SDL_SetRenderTarget(renderer, nullptr);
        letterboxed = beginLetterboxedFrame();
    }
    SDL_SetRenderViewport(renderer, nullptr);
    clearFrame();

    // Built-in render components contribute draw requests during the render
    // phase so frozen editor frames still show authored scene content.
    // GameLoop queues them with each tick instead, to interpolate them.
    ComponentManager::ResolveTransformHierarchy();
    if (queue_builtin_renderers) ComponentManager::QueueBuiltinRenderers();
    ParticleManager::QueueRenderBatches();

    const float zoom_factor = std::clamp(zoom, kMinZoomFactor, kMaxZoomFactor);
    const int render_width = std::max(1, config_.window_width);
    const int render_height = std::max(1, config_.window_height);
    int output_width = runtime_render_target_width_;
    int output_height = runtime_render_target_height_;
    if (letterboxed) {
        output_width = letterbox_target_width_;
        output_height = letterbox_target_height_;
    } else if (!render_runtime_to_texture_ || runtime_render_target_ == nullptr) {
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
    ComponentManager::QueueTilemaps(camera.x - half_world_width,
                                    camera.y - half_world_height,
                                    camera.x + half_world_width,
                                    camera.y + half_world_height);
    Renderer::RenderFrame(renderer, camera.x, camera.y,
                          zoom_factor, render_width, render_height,
                          output_scale_x, output_scale_y);
    RecordRuntimeRenderFrame();

    if (letterboxed) {
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_SetRenderViewport(renderer, nullptr);
        SDL_SetRenderScale(renderer, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        const SDL_FRect destination{game_viewport_.x, game_viewport_.y, game_viewport_.w, game_viewport_.h};
        SDL_RenderTexture(renderer, letterbox_target_, nullptr, &destination);
    }
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
