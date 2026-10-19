#include "RegistrationDetail.h"
#include "rendering/Renderer.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include "LuaBridge/Variant.h"
#include "core/Vector2.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

using APIRegistrationDetail::LuaState;

int DowncastToInt(float value) {
    return static_cast<int>(value);
}

int ClampByteInt(int value) {
    return std::clamp(value, 0, 255);
}

// The optional last argument of Image.Draw/DrawEx: a number or string naming
// what is drawn, so frames between gameplay ticks can move it smoothly.
using DrawKey = std::optional<std::variant<double, std::string_view>>;

std::uint64_t InterpolationKey(const DrawKey &key) {
    if (!key) return 0;
    if (const double *number = std::get_if<double>(&*key)) return Renderer::InterpolationKey(*number);
    return Renderer::InterpolationKey(std::get<std::string_view>(*key));
}

// The optional blend argument of Image.DrawEx/DrawUIScaled: nil or "normal"
// alpha blends, "add" adds the image's light to what is under it, "multiply"
// multiplies what is under it by the image's colour, letting its alpha through.
ImageBlend ParseBlend(const std::optional<std::string_view> &blend, const char *function) {
    if (!blend || *blend == "normal") return ImageBlend::Normal;
    if (*blend == "add") return ImageBlend::Add;
    if (*blend == "multiply") return ImageBlend::Multiply;
    throw std::invalid_argument(std::string(function) + " blend must be nil, \"normal\", \"add\" or \"multiply\"");
}

void CppImageDrawUI(const std::string &image_name, float x, float y) {
    ImageDrawRequest request;
    request.image_name = image_name;
    request.x = static_cast<float>(DowncastToInt(x));
    request.y = static_cast<float>(DowncastToInt(y));
    request.rotation_degrees = 0;
    request.scale_x = 1.0f;
    request.scale_y = 1.0f;
    request.pivot_x = 0.0f;
    request.pivot_y = 0.0f;
    request.r = 255;
    request.g = 255;
    request.b = 255;
    request.a = 255;
    request.sorting_order = 0;
    Renderer::QueueUIImageDraw(request);
}

void CppImageDrawUIEx(const std::string &image_name, float x, float y, float r,
                      float g, float b, float a, float sorting_order) {
    ImageDrawRequest request;
    request.image_name = image_name;
    request.x = static_cast<float>(DowncastToInt(x));
    request.y = static_cast<float>(DowncastToInt(y));
    request.rotation_degrees = 0;
    request.scale_x = 1.0f;
    request.scale_y = 1.0f;
    request.pivot_x = 0.0f;
    request.pivot_y = 0.0f;
    request.r = ClampByteInt(DowncastToInt(r));
    request.g = ClampByteInt(DowncastToInt(g));
    request.b = ClampByteInt(DowncastToInt(b));
    request.a = ClampByteInt(DowncastToInt(a));
    request.sorting_order = DowncastToInt(sorting_order);
    Renderer::QueueUIImageDraw(request);
}

// UI image scaled from its top-left corner (negative scales mirror).
void CppImageDrawUIScaled(const std::string &image_name, float x, float y, float scale_x, float scale_y,
                          float r, float g, float b, float a, float sorting_order,
                          std::optional<std::string_view> blend) {
    if (!std::isfinite(scale_x) || !std::isfinite(scale_y))
        throw std::invalid_argument("Image.DrawUIScaled scale must be finite");
    ImageDrawRequest request;
    request.image_name = image_name;
    request.x = static_cast<float>(DowncastToInt(x));
    request.y = static_cast<float>(DowncastToInt(y));
    request.scale_x = scale_x;
    request.scale_y = scale_y;
    request.pivot_x = 0.0f;
    request.pivot_y = 0.0f;
    request.r = ClampByteInt(DowncastToInt(r));
    request.g = ClampByteInt(DowncastToInt(g));
    request.b = ClampByteInt(DowncastToInt(b));
    request.a = ClampByteInt(DowncastToInt(a));
    request.sorting_order = DowncastToInt(sorting_order);
    request.blend = ParseBlend(blend, "Image.DrawUIScaled");
    Renderer::QueueUIImageDraw(request);
}

Vector2 CppImageGetSize(const std::string &image_name) {
    int width = 0, height = 0;
    if (!Renderer::GetImageSize(image_name, width, height))
        throw std::runtime_error("Image.GetSize: cannot load image '" + image_name + "'");
    return {static_cast<float>(width), static_cast<float>(height)};
}

void CppImageDraw(const std::string &image_name, float x, float y, DrawKey key) {
    ImageDrawRequest request;
    request.image_name = image_name;
    request.x = x;
    request.y = y;
    request.rotation_degrees = 0;
    request.scale_x = 1.0f;
    request.scale_y = 1.0f;
    request.pivot_x = 0.5f;
    request.pivot_y = 0.5f;
    request.r = 255;
    request.g = 255;
    request.b = 255;
    request.a = 255;
    request.sorting_order = 0;
    request.interpolation_key = InterpolationKey(key);
    Renderer::QueueSceneImageDraw(request);
}

void CppImageDrawEx(const std::string &image_name, float x, float y,
                    float rotation_degrees, float scale_x, float scale_y,
                    float pivot_x, float pivot_y, float r, float g, float b,
                    float a, float sorting_order, DrawKey key,
                    std::optional<std::string_view> blend) {
    ImageDrawRequest request;
    request.image_name = image_name;
    request.x = x;
    request.y = y;
    request.rotation_degrees = rotation_degrees;
    request.scale_x = scale_x;
    request.scale_y = scale_y;
    request.pivot_x = pivot_x;
    request.pivot_y = pivot_y;
    request.r = ClampByteInt(DowncastToInt(r));
    request.g = ClampByteInt(DowncastToInt(g));
    request.b = ClampByteInt(DowncastToInt(b));
    request.a = ClampByteInt(DowncastToInt(a));
    request.sorting_order = DowncastToInt(sorting_order);
    request.interpolation_key = InterpolationKey(key);
    request.blend = ParseBlend(blend, "Image.DrawEx");
    Renderer::QueueSceneImageDraw(request);
}

void CppImageDrawPixel(float x, float y, float r, float g, float b, float a) {
    PixelDrawRequest request;
    request.x = DowncastToInt(x);
    request.y = DowncastToInt(y);
    request.r = ClampByteInt(DowncastToInt(r));
    request.g = ClampByteInt(DowncastToInt(g));
    request.b = ClampByteInt(DowncastToInt(b));
    request.a = ClampByteInt(DowncastToInt(a));
    Renderer::QueuePixelDraw(request);
}

// Image.SetSmoothing(smooth): image files drawn scaled sample linearly instead of nearest.
void CppImageSetSmoothing(bool smooth) { Renderer::SetSmoothImages(smooth); }
bool CppImageGetSmoothing() { return Renderer::SmoothImages(); }

void InjectImageAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Image")
        .addFunction("DrawUI", &CppImageDrawUI)
        .addFunction("DrawUIEx", &CppImageDrawUIEx)
        .addFunction("DrawUIScaled", &CppImageDrawUIScaled)
        .addFunction("GetSize", &CppImageGetSize)
        .addFunction("Draw", &CppImageDraw)
        .addFunction("DrawEx", &CppImageDrawEx)
        .addFunction("DrawPixel", &CppImageDrawPixel)
        .addFunction("SetSmoothing", &CppImageSetSmoothing)
        .addFunction("GetSmoothing", &CppImageGetSmoothing)
        .endNamespace();
}

void CppTextDraw(const std::string &str_content, float x, float y,
                 const std::string &font_name, float font_size, float r, float g,
                 float b, float a) {
    const int x_i = static_cast<int>(x);
    const int y_i = static_cast<int>(y);
    const int font_size_i = static_cast<int>(font_size);
    const int r_i = static_cast<int>(r);
    const int g_i = static_cast<int>(g);
    const int b_i = static_cast<int>(b);
    const int a_i = static_cast<int>(a);

    SDL_Color color = {static_cast<Uint8>(r_i), static_cast<Uint8>(g_i),
                       static_cast<Uint8>(b_i), static_cast<Uint8>(a_i)};
    Renderer::DrawText(str_content, font_name, font_size_i, color, x_i, y_i);
}

void InjectTextAPI() {
    luabridge::getGlobalNamespace(LuaState())
        .beginNamespace("Text")
        .addFunction("Draw", &CppTextDraw)
        .endNamespace();
}

} // namespace

namespace APIRegistrationDetail {

void RegisterRenderingAPI() {
    InjectImageAPI();
    InjectTextAPI();
}

} // namespace APIRegistrationDetail
