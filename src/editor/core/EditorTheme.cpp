#include "editor/core/EditorTheme.h"

#include <algorithm>

namespace {

constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                  static_cast<float>(b) / 255.0f, a);
}

ImVec4 Mix(const ImVec4 &a, const ImVec4 &b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

ImVec4 Alpha(const ImVec4 &c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// Unity's Professional (dark) skin.
const EditorPalette kDark{
    Rgb(56, 56, 56),    Rgb(40, 40, 40),    Rgb(42, 42, 42),    Rgb(70, 70, 70),
    Rgb(25, 25, 25),    Rgb(210, 210, 210), Rgb(128, 128, 128), Rgb(58, 121, 187),
    Rgb(44, 93, 135),   Rgb(48, 48, 48),    Rgb(38, 72, 104),   Rgb(104, 74, 34),
    Rgb(230, 234, 240), false};

// Unity's Personal (light) skin.
const EditorPalette kLight{
    Rgb(200, 200, 200), Rgb(165, 165, 165), Rgb(240, 240, 240), Rgb(228, 228, 228),
    Rgb(153, 153, 153), Rgb(16, 16, 16),    Rgb(112, 112, 112), Rgb(58, 114, 176),
    Rgb(143, 180, 224), Rgb(214, 214, 214), Rgb(120, 162, 214), Rgb(222, 170, 96),
    Rgb(20, 24, 30),    true};

// A blue-grey night theme.
const EditorPalette kMidnight{
    Rgb(30, 32, 48),    Rgb(24, 26, 38),    Rgb(36, 39, 58),    Rgb(47, 51, 73),
    Rgb(19, 20, 30),    Rgb(200, 211, 245), Rgb(115, 122, 162), Rgb(122, 162, 247),
    Rgb(45, 63, 118),   Rgb(36, 39, 58),    Rgb(45, 63, 118),   Rgb(110, 80, 40),
    Rgb(220, 228, 250), false};

const EditorPalette *g_current = &kMidnight;

const EditorPalette &Palette(std::string_view name) {
    if (name == "light") return kLight;
    if (name == "dark") return kDark;
    return kMidnight;
}

void ApplyColors(ImGuiStyle &style, const EditorPalette &p) {
    ImVec4 *c = style.Colors;
    // Hover and press states move toward the text colour (gentler on light).
    auto hover = [&](const ImVec4 &base, float amount) { return Mix(base, p.text, amount * (p.light ? 0.5f : 1.0f)); };

    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.text_dim;
    c[ImGuiCol_WindowBg] = p.window;
    c[ImGuiCol_ChildBg] = Alpha(p.window, 0.0f);
    c[ImGuiCol_PopupBg] = Alpha(Mix(p.window, p.header, 0.5f), 0.98f);
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = p.frame;
    c[ImGuiCol_FrameBgHovered] = hover(p.frame, 0.08f);
    c[ImGuiCol_FrameBgActive] = hover(p.frame, 0.14f);
    c[ImGuiCol_TitleBg] = p.header;
    c[ImGuiCol_TitleBgActive] = p.header;
    c[ImGuiCol_TitleBgCollapsed] = p.header;
    c[ImGuiCol_MenuBarBg] = p.header;
    c[ImGuiCol_ScrollbarBg] = Alpha(p.window, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = p.button;
    c[ImGuiCol_ScrollbarGrabHovered] = hover(p.button, 0.12f);
    c[ImGuiCol_ScrollbarGrabActive] = hover(p.button, 0.22f);
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent;
    c[ImGuiCol_SliderGrabActive] = Mix(p.accent, p.text, 0.25f);
    c[ImGuiCol_Button] = p.button;
    c[ImGuiCol_ButtonHovered] = hover(p.button, 0.10f);
    c[ImGuiCol_ButtonActive] = p.selection;
    c[ImGuiCol_Header] = p.selection;
    c[ImGuiCol_HeaderHovered] = hover(p.window, 0.08f);
    c[ImGuiCol_HeaderActive] = Mix(p.selection, p.accent, 0.4f);
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_SeparatorHovered] = p.accent;
    c[ImGuiCol_SeparatorActive] = p.accent;
    c[ImGuiCol_ResizeGrip] = Alpha(p.accent, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = Alpha(p.accent, 0.6f);
    c[ImGuiCol_ResizeGripActive] = p.accent;
    c[ImGuiCol_InputTextCursor] = p.text;
    c[ImGuiCol_TabHovered] = hover(p.window, 0.10f);
    c[ImGuiCol_Tab] = p.header;
    c[ImGuiCol_TabSelected] = p.window;
    c[ImGuiCol_TabSelectedOverline] = p.accent;
    c[ImGuiCol_TabDimmed] = p.header;
    c[ImGuiCol_TabDimmedSelected] = p.window;
    c[ImGuiCol_TabDimmedSelectedOverline] = Alpha(p.accent, 0.0f);
    c[ImGuiCol_DockingPreview] = Alpha(p.accent, 0.45f);
    c[ImGuiCol_DockingEmptyBg] = p.header;
    c[ImGuiCol_PlotLines] = p.text_dim;
    c[ImGuiCol_PlotLinesHovered] = p.accent;
    c[ImGuiCol_PlotHistogram] = p.accent;
    c[ImGuiCol_PlotHistogramHovered] = Mix(p.accent, p.text, 0.25f);
    c[ImGuiCol_TableHeaderBg] = p.header;
    c[ImGuiCol_TableBorderStrong] = p.border;
    c[ImGuiCol_TableBorderLight] = Mix(p.window, p.border, 0.5f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = Alpha(p.text, 0.03f);
    c[ImGuiCol_TextLink] = p.accent;
    c[ImGuiCol_TextSelectedBg] = Alpha(p.accent, 0.35f);
    c[ImGuiCol_TreeLines] = p.border;
    c[ImGuiCol_DragDropTarget] = p.accent;
    c[ImGuiCol_DragDropTargetBg] = Alpha(p.accent, 0.15f);
    c[ImGuiCol_UnsavedMarker] = p.text;
    c[ImGuiCol_NavCursor] = p.accent;
    c[ImGuiCol_NavWindowingHighlight] = Alpha(p.text, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = Alpha(p.border, 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = Alpha(p.border, 0.35f);
}

}  // namespace

namespace EditorTheme {

const std::vector<std::string> &Names() {
    static const std::vector<std::string> names = {"midnight", "dark", "light"};
    return names;
}

const char *DisplayName(std::string_view name) {
    if (name == "light") return "Light (Unity Personal)";
    if (name == "dark") return "Dark (Unity Professional)";
    return "Midnight";
}

std::string Normalize(std::string_view name) {
    const auto &names = Names();
    return std::find(names.begin(), names.end(), name) != names.end() ? std::string(name) : "midnight";
}

void Apply(ImGuiStyle &style, std::string_view name, float ui_scale) {
    style = ImGuiStyle();
    const EditorPalette &palette = Palette(name);
    g_current = &palette;

    // Compact, flat panels with thin borders, soft rounded controls and an
    // accent line on the selected tab.
    style.WindowPadding = ImVec2(8.0f, 8.0f);
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 3.0f);
    style.IndentSpacing = 14.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    style.DockingSeparatorSize = 2.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;
    style.ScaleAllSizes(ui_scale);
    ApplyColors(style, palette);
}

const EditorPalette &Current() { return *g_current; }

}  // namespace EditorTheme
