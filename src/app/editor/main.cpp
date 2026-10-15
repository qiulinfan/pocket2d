#include "app/editor/EditorApp.h"
#include <SDL3/SDL_main.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    EditorApp app;
    app.Run();
    return 0;
}
