#ifndef ENGINE_SPLASH_SCREEN_H
#define ENGINE_SPLASH_SCREEN_H

struct SDL_Renderer;

// The optional "Made with Pocket2D" splash (game.config "splash_screen"),
// shown by the standalone runtime before the first frame, like Unity's
// splash screen. The image is compiled into the runtime from
// .engine/system/splash.png (tools/make_splash.py draws it).
namespace SplashScreen {
// Fades the logo in and out over `seconds` on a dark background, keeping the
// window responsive. Returns false when the window was closed meanwhile.
bool Show(SDL_Renderer *renderer, float seconds);
}  // namespace SplashScreen

#endif
