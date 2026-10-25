// The editor's play World runs in its own Engine embedded in the edit Engine's
// window and renderer. Play must not touch the edit World, and Stop must
// leave the host renderer usable without reloading the edit World.
#include "core/Engine.h"
#include "core/FrameClock.h"
#include "core/World.h"
#include "editor/core/EditorSceneSession.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

int Ticks(Engine &engine) {
    WorldScope scope(engine.GetWorld());
    return ComponentManager::GetComponentByKey(1, "logic")["ticks"].cast<int>().value();
}

std::unique_ptr<Engine> StartPlay(World &world, Engine &host, EditorSceneSession &session) {
    auto play = std::make_unique<Engine>(world);
    play->InitializeEmbedded(host.GetWindow(), host.GetRenderer());
    play->SetRenderRuntimeToTexture(true);
    play->SetRuntimeRenderTargetSize(320, 240);
    Check(session.EnterPlayMode(*play), "enter play: " + play->GetLastSceneLoadError());
    return play;
}

void Run() {
    Engine host;
    host.InitializeRuntime();
    SDL_HideWindow(host.GetWindow());
    host.SetRenderRuntimeToTexture(true);
    host.SetRuntimeRenderTargetSize(320, 240);
    EditorSceneSession session;
    session.LoadInitialScene(host);
    session.SyncRuntimeMirrorIfDirty(host);
    host.RunRenderFrozenFrame(false, false);
    host.PresentFrame(false);
    const int host_ticks = Ticks(host);
    const int host_frame = FrameClock::GetFrameNumber();

    {
        World play_world;
        auto play = StartPlay(play_world, host, session);
        for (int i = 0; i < 30; ++i) {
            play->RunSingleFrame(false);
            play->PresentFrame(true);
        }
        Check(Ticks(*play) >= 29, "play World did not simulate");
        Check(Ticks(host) == host_ticks && FrameClock::GetFrameNumber() == host_frame,
              "play advanced the edit World");
        Check(play->GetRuntimeRenderTarget() != nullptr &&
                  play->GetRuntimeRenderTarget() != host.GetRuntimeRenderTarget(),
              "play World must render into its own target");
        Check(session.GetAuthoringSceneDocument().SetActorName(5, "edited_during_play"),
              "authoring draft must stay editable during play");
        Check(session.EndPlaySession(host) && !session.IsPlayModeActive(), "stop play");
        Check(session.IsRuntimeMirrorPending(), "a draft edited during play must refresh the edit World");
        play->ShutdownRuntime();
    }

    // The host renderer, window and edit World survive the play Engine.
    Check(host.GetRenderer() != nullptr && host.GetWindow() != nullptr, "play shutdown released host presenter");
    session.SyncRuntimeMirrorIfDirty(host);
    host.RunRenderFrozenFrame(false, false);
    host.PresentFrame(false);
    Check(!session.IsRuntimeMirrorPending() && host.GetRuntimeActorByUID(6)->actor_name == "edited_during_play",
          "edit World did not take the draft edited during play");
    Check(Ticks(host) == host_ticks, "edit World simulated while playing");

    {
        World play_world;
        auto play = StartPlay(play_world, host, session);
        play->RunSingleFrame(false);
        Check(session.EndPlaySession(host) && !session.IsRuntimeMirrorPending(),
              "an untouched draft must not reload the edit World after play");
        play->ShutdownRuntime();
    }
    host.RunRenderFrozenFrame(false, false);
    host.PresentFrame(false);
}
} // namespace

int main(int argc, char **argv) {
    try {
        Check(argc == 2, "usage: editor_play_world_test FIXTURE_PROJECT");
        SDL_setenv_unsafe("SDL_VIDEO_DRIVER", "dummy", 1);
        SDL_setenv_unsafe("SDL_AUDIO_DRIVER", "dummy", 1);
        SDL_setenv_unsafe("POCKET_RENDER_DRIVER", "software", 1);
        SDL_setenv_unsafe("POCKET_RENDER_VSYNC", "0", 1);
        ResourcePath::SetResourcesRootPath(argv[1]);
        Run();
        std::cout << "editor_play_world_test: separate play World passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "editor_play_world_test: " << error.what() << '\n';
        return 1;
    }
}
