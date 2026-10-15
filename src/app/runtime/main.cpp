#include "app/runtime/RuntimeApp.h"
#include <SDL3/SDL_main.h>

int main(int argc, char *argv[]) {
    RuntimeApp app;
    return app.Run(argc, argv);
}
