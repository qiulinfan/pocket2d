#ifndef ENGINE_FRAME_RATE_OVERLAY_H
#define ENGINE_FRAME_RATE_OVERLAY_H

#include <SDL3/SDL_stdinc.h>

struct SDL_Renderer;

// The standalone runtime's frame-rate readout, toggled with F3 and shown from
// the start by game.config "show_fps": frames per second, and the time the
// engine spent on a frame (input, gameplay, physics and drawing, but not the
// wait for the next 1/60 s tick) on average and at worst. The figures cover
// the last half second. It draws with SDL's built-in debug font, at the
// bottom center of the window (games keep their HUD in the corners and along
// the top), so it needs no font file.
class FrameRateOverlay {
public:
    bool visible = false;

    // Records a frame that took `work_ns` before presenting, at time `now_ns`.
    void RecordFrame(Uint64 work_ns, Uint64 now_ns);
    void Draw(SDL_Renderer *renderer) const;

    float Fps() const { return fps_; }
    float AverageMs() const { return average_ms_; }
    float WorstMs() const { return worst_ms_; }

private:
    Uint64 window_start_ns_ = 0;
    int frames_ = 0;
    Uint64 work_total_ns_ = 0;
    Uint64 work_worst_ns_ = 0;
    bool measured_ = false;
    float fps_ = 0.0f;
    float average_ms_ = 0.0f;
    float worst_ms_ = 0.0f;
};

#endif
