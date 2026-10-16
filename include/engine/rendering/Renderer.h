#ifndef RENDERER_H
#define RENDERER_H

#include "SDL3/SDL.h"
#include "SDL3_ttf/SDL_ttf.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

struct ImageDrawRequest {
    // basic image name and world-space transform
    // 基本图片名和世界坐标变换
    std::string image_name;
    // Renderer::InternImage id; when nonzero it names the image instead of
    // image_name, so per-frame requests need not copy strings.
    std::uint32_t image_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float rotation_degrees = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;

    // normalized pivot and RGBA modulation
    // 归一化 pivot 和 RGBA 调制
    float pivot_x = 0.5f;
    float pivot_y = 0.5f;
    int r = 255;
    int g = 255;
    int b = 255;
    int a = 255;
    int sorting_order = 0;
    // Secondary draw key within one sorting_order (ascending; world y for
    // auto-sorted sprites, so lower sprites draw in front). Ties keep
    // submission order.
    float sort_y = 0.0f;

    // Optional spritesheet selection. Rows/columns describe the project-level
    // grid metadata; row/column select one cell using 1-based indexing.
    int spritesheet_rows = 1;
    int spritesheet_columns = 1;
    int sprite_row = 1;
    int sprite_column = 1;
};

// (sorting_order, sort_y) packed into one sortable integer, plus the request
// index that breaks ties in submission order.
struct DrawOrderKey {
    std::uint64_t key = 0;
    std::uint32_t index = 0;
    friend bool operator<(const DrawOrderKey &a, const DrawOrderKey &b) {
        return a.key != b.key ? a.key < b.key : a.index < b.index;
    }
};

struct PixelDrawRequest {
    int x = 0;
    int y = 0;
    int r = 255;
    int g = 255;
    int b = 255;
    int a = 255;
};

struct TextDrawRequest {
    // text content, font choice, and screen position
    // 文本内容, 字体选择和屏幕坐标
    std::string text;
    std::string font_name;
    int font_size = 16;
    SDL_Color color = {255, 255, 255, 255};
    int x = 0;
    int y = 0;
};

// Laid-out text drawn from the text engine's shared glyph atlas.
struct CachedTextTexture {
    TTF_Text *text = nullptr;
    Uint32 last_used_timestamp = 0;
};

struct ParticleBatchRequest {
    int emitter_id = -1;
    int sorting_order = 0;
};

struct RenderQueueState;

class Renderer {
public:
    // used when particles do not specify an image
    // 粒子没有指定图片时使用
    static inline constexpr const char *kDefaultParticleTextureName = "__default_particle__";

    static void Init();
    static void Shutdown();
    // Release this tick's presentation requests when simulation runs headless.
    static void DiscardFrameRequests();

    // resource loading and draw queue entry points
    // 资源加载和绘制队列入口
    // A stable, process-wide nonzero id for an image name. Thread-safe.
    static std::uint32_t InternImage(const std::string &image_name);
    static SDL_Texture *LoadTexture(const std::string &image_name,
                                    SDL_Renderer *renderer);
    static SDL_Texture *LoadTexture(const std::string &image_name,
                                    SDL_Renderer *renderer, float *width,
                                    float *height);
    // Pixel size of an image resource, decoded without a renderer (headless
    // runs too) and cached until images reload. False when it cannot load.
    static bool GetImageSize(const std::string &image_name, int &width, int &height);
    static void QueueSceneImageDraw(const ImageDrawRequest &request);
    static void QueueSceneParticleBatch(const ParticleBatchRequest &request);
    static void QueueUIImageDraw(const ImageDrawRequest &request);
    static void QueuePixelDraw(const PixelDrawRequest &request);

    // text draw helpers
    // 文本绘制辅助接口
    static void DrawText(const std::string &text_content,
                         const std::string &font_name, int font_size,
                         SDL_Color font_color, int x, int y);

    static void RenderFrame(SDL_Renderer *renderer, float camera_x,
                            float camera_y, float zoom_factor,
                            int camera_width, int camera_height,
                            float output_scale_x = 1.0f,
                            float output_scale_y = 1.0f);
    static void ClearCache();
    // Drops cached textures whose image file changed or disappeared since it
    // was loaded, so the next draw reloads them. Returns how many.
    static int ReloadChangedImages();
    static int GetResourceWarningCount();
    static const std::string &GetLastResourceWarning();

private:
    struct CachedImageTexture {
        SDL_Texture *texture = nullptr;
        float width = 0.0f;
        float height = 0.0f;
        // Image file and its modification time when loaded; empty for
        // generated textures.
        std::filesystem::path source;
        std::filesystem::file_time_type modified{};
        bool missing = false; // placeholder for an image that was not found
    };

    static CachedImageTexture *LoadTextureResource(
        const std::string &image_name, SDL_Renderer *renderer);
    static CachedImageTexture *LoadTextureResource(const ImageDrawRequest &request, SDL_Renderer *renderer);
    // Clears the texture cache when the project root changed; once per frame.
    static void SyncImageCacheRoot();
    static void ClearImageTextureCache();
    static TTF_Font *GetFont(const std::string &font_name, int size);
    static void PruneTextCache();
    static void RenderAndClearAllImages(SDL_Renderer *renderer, float camera_x,
                                        float camera_y, float zoom_factor,
                                        int camera_width, int camera_height,
                                        float output_scale_x,
                                        float output_scale_y);
    static void RenderAndClearAllText(SDL_Renderer *renderer,
                                      float output_scale_x,
                                      float output_scale_y);
    static void RenderAndClearAllPixels(SDL_Renderer *renderer);

    // caches and per-frame draw queues
    // 资源缓存和逐帧绘制队列
    static inline bool initialized = false;
    static inline std::filesystem::path::string_type image_cache_resources_root;
    static inline std::unordered_map<std::string, CachedImageTexture> image_cache;
    // InternImage id -> image_cache entry, filled on first use; render thread only.
    static inline std::vector<CachedImageTexture *> image_cache_by_id;
    static inline std::unordered_map<std::string, std::unordered_map<int, TTF_Font *>>
        font_cache;
    static inline std::unordered_map<std::string, CachedTextTexture> text_cache;
    // SDL_ttf renderer text engine for text_engine_renderer; recreated with
    // an empty text cache when text is drawn with another renderer.
    static inline TTF_TextEngine *text_engine = nullptr;
    static inline SDL_Renderer *text_engine_renderer = nullptr;
    static void ResetTextEngine();
    // Per-World draw queues; see RenderQueueState below. The resource caches
    // above are process-wide and used only on the thread owning the renderer.
    static RenderQueueState &Queues();
};

// Draw requests queued by one World's gameplay during a frame. Renderer's
// static API reads and writes the queues of World::Current().
struct RenderQueueState {
    std::vector<ImageDrawRequest> scene_draw_requests;
    std::vector<ParticleBatchRequest> scene_particle_batches;
    std::vector<ImageDrawRequest> ui_draw_requests;
    // Draw order scratch, reused across frames; see BuildDrawOrder.
    std::vector<DrawOrderKey> scene_draw_order;
    std::vector<DrawOrderKey> ui_draw_order;
    std::vector<PixelDrawRequest> pixel_draw_requests;
    std::vector<TextDrawRequest> text_draw_requests;
};

#endif
