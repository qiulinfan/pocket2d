#ifndef SHARED_GAME_CONFIG_H
#define SHARED_GAME_CONFIG_H

#include <string>

struct GameConfigData {
    // Runtime window resolution and clear-color defaults used by both the
    // standalone runtime app and the editor-hosted runtime viewport.
    int window_width = 640;
    int window_height = 360;

    int clear_color_r = 255;
    int clear_color_g = 255;
    int clear_color_b = 255;

    float zoom_factor = 1.0f;

    std::string game_title = "";
    std::string save_namespace = "";
    std::string initial_scene_name = "";

    // Optional "Made with Pocket2D" splash before the first frame of the
    // standalone runtime (game.config "splash_screen", "splash_seconds").
    bool splash_screen = false;
    float splash_seconds = 2.0f;
    // Whether the standalone runtime starts with its frame-rate readout shown
    // (game.config "show_fps"; F3 toggles it while playing).
    bool show_fps = false;
    // The standalone runtime renders as often as it can while gameplay ticks
    // 60 times a second. max_fps (1..1000; 0 = no limit) caps the frame rate;
    // vsync waits for the display's refresh instead.
    int max_fps = 0;
    bool vsync = false;
    // Frames drawn between two ticks move the camera and keyed draws part of
    // the way to the latest tick (one tick behind it); false shows the latest
    // tick as it is.
    bool frame_interpolation = true;
};

class GameConfig {
public:
    // Read game.config and rendering.config into one lightweight shared value
    // object. Engine owns the behavior; this type only describes startup data.
    static GameConfigData Read();
    // Persist project startup config immediately back into game.config and
    // rendering.config. This is used by the editor settings UI and does not
    // hot-reload the currently running runtime instance.
    static bool Write(const GameConfigData &config);
};

#endif
