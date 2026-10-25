#ifndef EDITOR_THEME_H
#define EDITOR_THEME_H

#include "imgui.h"
#include <string>
#include <string_view>
#include <vector>

// Editor colour themes. A theme is a small palette; Apply derives every
// Dear ImGui colour and the editor's spacing from it, and panels that draw
// their own shapes (Project tiles, Hierarchy badges) read the same palette.
struct EditorPalette {
    ImVec4 window;     // panel background
    ImVec4 header;     // title bars, menu bar, inactive tabs, table headers
    ImVec4 frame;      // input fields and wells behind images
    ImVec4 button;
    ImVec4 border;
    ImVec4 text;
    ImVec4 text_dim;
    ImVec4 accent;     // focus, checks, active tab overline
    ImVec4 selection;  // selected rows and tiles
    ImVec4 tile;       // Project browser tiles
    ImVec4 badge_scene;
    ImVec4 badge_runtime;
    ImVec4 badge_text;
    bool light = false;
};

namespace EditorTheme {
// Theme ids accepted in editor.config ("theme"), in menu order.
const std::vector<std::string> &Names();
// A short label for the settings combo.
const char *DisplayName(std::string_view name);
// Known theme or "midnight" (the default).
std::string Normalize(std::string_view name);
// Colours and spacing for the theme, scaled for the UI scale.
void Apply(ImGuiStyle &style, std::string_view name, float ui_scale);
// The palette applied last (midnight before the first Apply).
const EditorPalette &Current();
}  // namespace EditorTheme

#endif
