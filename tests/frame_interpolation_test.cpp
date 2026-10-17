// Frames drawn between two gameplay ticks: keyed scene draws pair up with the
// previous tick's and are placed part of the way between them; everything
// else shows the latest tick.
#include "core/World.h"
#include "rendering/Renderer.h"
#include "scripting/APIRegistration.h"
#include "lua.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Check(bool condition, const char *message) {
    if (condition) return;
    std::fprintf(stderr, "frame_interpolation_test: %s\n", message);
    std::exit(1);
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

ImageDrawRequest Draw(float x, float y, std::uint64_t key, float rotation = 0.0f, float scale_x = 1.0f) {
    ImageDrawRequest request;
    request.image_name = "image.png";
    request.x = x;
    request.y = y;
    request.rotation_degrees = rotation;
    request.scale_x = scale_x;
    request.interpolation_key = key;
    return request;
}

// The scene requests a frame drawn at alpha would show.
std::vector<ImageDrawRequest> Frame(float alpha, float snap_distance = 100.0f) {
    Renderer::RequeueHeldFrameRequests(alpha, snap_distance);
    std::vector<ImageDrawRequest> frame = World::Current().State<RenderQueueState>().scene_draw_requests;
    Renderer::DiscardFrameRequests();
    return frame;
}

bool Lua(lua_State *state, const char *code) {
    const bool ok = luaL_dostring(state, code) == LUA_OK;
    lua_settop(state, 0);
    return ok;
}

void Tick(const std::vector<ImageDrawRequest> &draws, bool match_previous = true) {
    for (const ImageDrawRequest &draw : draws) Renderer::QueueSceneImageDraw(draw);
    Renderer::HoldFrameRequests(match_previous);
}
}  // namespace

int main() {
    const std::uint64_t player = Renderer::InterpolationKey(1.0);
    const std::uint64_t enemy = Renderer::InterpolationKey("enemy");
    const std::uint64_t spawned = Renderer::InterpolationKey("spawned");
    Check(player != 0 && enemy != 0 && Renderer::ActorInterpolationKey(1) != 0, "keys are never 0 (no key)");
    Check(player != Renderer::InterpolationKey("1") && player != Renderer::ActorInterpolationKey(1),
          "numbers, strings and actors are separate keys");
    Check(Renderer::InterpolationKey(-0.0) == Renderer::InterpolationKey(0.0), "-0 and 0 are one key");

    Tick({Draw(0, 0, player), Draw(10, 0, enemy, 350), Draw(5, 5, 0)});
    Tick({Draw(2, 4, player), Draw(12, 0, enemy, 10), Draw(7, 5, 0), Draw(20, 0, spawned)});
    std::vector<ImageDrawRequest> frame = Frame(0.25f);
    Check(frame.size() == 4, "every held request is drawn");
    Check(Near(frame[0].x, 0.5f) && Near(frame[0].y, 1.0f), "a keyed draw moves a quarter of the way");
    Check(Near(frame[1].x, 10.5f) && Near(frame[1].rotation_degrees, 355.0f), "rotation turns the short way");
    Check(Near(frame[2].x, 7.0f), "an unkeyed draw shows the latest tick");
    Check(Near(frame[3].x, 20.0f), "a key new this tick shows the latest tick");
    frame = Frame(1.0f);
    Check(Near(frame[0].x, 2.0f) && Near(frame[1].rotation_degrees, 10.0f), "alpha 1 is the latest tick");
    frame = Frame(0.25f);
    Check(Near(frame[0].x, 0.5f), "every frame until the next tick interpolates from the held ticks");

    // Draws sharing a key pair up in submission order; a teleport, a mirrored
    // flip and a sudden turn are not smoothed.
    Tick({Draw(0, 0, enemy), Draw(100, 0, enemy), Draw(0, 0, player, 30, 1), Draw(0, 0, spawned, 0, 2)});
    Tick({Draw(4, 0, enemy), Draw(500, 0, enemy), Draw(4, 0, player, 50, -1), Draw(4, 0, spawned, 170, 4)});
    frame = Frame(0.5f);
    Check(Near(frame[0].x, 2.0f), "the first draw of a key pairs with the previous first");
    Check(Near(frame[1].x, 500.0f), "a jump beyond the snap distance shows the latest tick");
    Check(Near(frame[2].x, 2.0f) && Near(frame[2].rotation_degrees, 50.0f) && Near(frame[2].scale_x, -1.0f),
          "a flipped image moves but does not turn");
    Check(Near(frame[3].x, 2.0f) && Near(frame[3].rotation_degrees, 170.0f) && Near(frame[3].scale_x, 4.0f),
          "a turn of more than 90 degrees in a tick is not smoothed");
    Tick({Draw(0, 0, player, 0, 1)});
    Tick({Draw(0, 0, player, 0, 3)});
    Check(Near(Frame(0.5f)[0].scale_x, 2.0f), "scale changes smoothly");

    // After a scene load the previous tick is forgotten.
    Tick({Draw(50, 0, player)}, false);
    Check(Near(Frame(0.0f)[0].x, 50.0f), "an unmatched tick shows the latest");
    Renderer::DiscardFrameRequests();

    // Lua names a draw with an optional last argument, a number or a string.
    lua_State *state = luaL_newstate();
    luaL_openlibs(state);
    APIRegistration::Initialize(state);
    Check(Lua(state, R"(
        Image.Draw("a.png", 1, 2)
        Image.Draw("a.png", 1, 2, 1)
        Image.DrawEx("a.png", 1, 2, 0, 1, 1, 0.5, 0.5, 255, 255, 255, 255, 0)
        Image.DrawEx("a.png", 1, 2, 0, 1, 1, 0.5, 0.5, 255, 255, 255, 255, 0, "enemy")
    )"), "Image.Draw and DrawEx take an optional key");
    const std::vector<ImageDrawRequest> &queued = World::Current().State<RenderQueueState>().scene_draw_requests;
    Check(queued.size() == 4 && queued[0].interpolation_key == 0 && queued[1].interpolation_key == player &&
              queued[2].interpolation_key == 0 && queued[3].interpolation_key == enemy,
          "Lua keys are the renderer's keys");
    Check(!Lua(state, R"(Image.Draw("a.png", 1, 2, {}))"), "a key that is not a number or string is an error");
    lua_close(state);
    return 0;
}
