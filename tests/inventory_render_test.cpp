#include "core/Engine.h"
#include "input/Input.h"
#include "scripting/ComponentManager.h"
#include "shared/resources/ResourcePath.h"
#include "SDL3_image/SDL_image.h"
#include "lua.hpp"
#include "LuaBridge/LuaBridge.h"
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
namespace {
using Surface=std::unique_ptr<SDL_Surface,decltype(&SDL_DestroySurface)>;
using Color=std::array<Uint8,4>;
void Check(bool condition,const std::string &message) {if(!condition) throw std::runtime_error(message+": "+SDL_GetError());}
Color Pixel(SDL_Surface *surface,int x,int y) {
    Color c{};Check(SDL_ReadSurfacePixel(surface,x,y,&c[0],&c[1],&c[2],&c[3]),"read inventory pixel");return c;
}
Surface Read(Engine &engine) {
    Check(ComponentManager::GetRuntimeErrorCount()==0,ComponentManager::GetLastRuntimeError());
    Check(SDL_SetRenderTarget(engine.GetRenderer(),engine.GetRuntimeRenderTarget()),"select inventory target");
    Surface result(SDL_RenderReadPixels(engine.GetRenderer(),nullptr),SDL_DestroySurface);
    Check(result!=nullptr&&result->w==960&&result->h==720,"read inventory target");
    Check(SDL_SetRenderTarget(engine.GetRenderer(),nullptr),"restore inventory target");return result;
}
void Key(Engine &engine,SDL_Scancode key,bool down) {
    SDL_Event event{};event.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;event.key.scancode=key;event.key.down=down;
    Input::ProcessEvent(event);engine.RunSingleFrame(false);
}
void Mouse(Engine &engine,float x,float y,bool down) {
    SDL_Event motion{};motion.type=SDL_EVENT_MOUSE_MOTION;motion.motion.x=x;motion.motion.y=y;Input::ProcessEvent(motion);
    SDL_Event button{};button.type=down?SDL_EVENT_MOUSE_BUTTON_DOWN:SDL_EVENT_MOUSE_BUTTON_UP;
    button.button.button=1;button.button.down=down;button.button.x=x;button.button.y=y;Input::ProcessEvent(button);
    engine.RunSingleFrame(false);
}
}
int main(int argc,char **argv) {
    const std::string backend=argc>2?argv[2]:"software";
    try {
        Check(argc>=2,"inventory fixture argument required");ResourcePath::SetResourcesRootPath(argv[1]);
        SDL_setenv_unsafe("POCKET_RENDER_DRIVER",backend.c_str(),1);SDL_setenv_unsafe("POCKET_RENDER_VSYNC","0",1);
        Engine engine;Check(engine.GetLastSceneLoadError().empty(),engine.GetLastSceneLoadError());
        engine.InitializeRuntime();Check(std::string(SDL_GetRendererName(engine.GetRenderer()))==backend,"requested renderer selected");
        Input::SetExternalInputEnabled(true);engine.SetRenderRuntimeToTexture(true);engine.SetRuntimeRenderTargetSize(960,720);
        engine.RunSingleFrame(false);auto initial=Read(engine);
        const Color boots{183,124,70,255},blank{21,29,36,255},blade{220,230,238,255};
        Check(Pixel(initial.get(),168,222)==boots,"bag contains visible boots");
        Check(Pixel(initial.get(),686,442)==blank,"legs slot initially empty");
        Check(Pixel(initial.get(),526,222)==blade,"chest contains visible sword");
        int player_pixels=0;
        for(int y=70;y<180;++y) for(int x=0;x<960;++x) if(Pixel(initial.get(),x,y)==Color{240,196,150,255})++player_pixels;
        Check(player_pixels>10,"world player visible above inventory UI");
        Key(engine,SDL_SCANCODE_3,true);Key(engine,SDL_SCANCODE_3,false);
        Key(engine,SDL_SCANCODE_U,true);Key(engine,SDL_SCANCODE_U,false);
        auto equipped=Read(engine);
        Check(Pixel(equipped.get(),168,222)==blank,"equipping boots clears bag icon");
        Check(Pixel(equipped.get(),686,442)==boots,"equipping boots draws legs icon");
        Check(Pixel(equipped.get(),168,562)==blank,"hotbar reflects same emptied bag slot");
        // Real mouse drag produces a preview and only transfers on release.
        Mouse(engine,526,236,true);auto dragging=Read(engine);
        Check(Pixel(dragging.get(),526,442)==blank,"drag preview does not equip early");
        Mouse(engine,526,456,false);auto moved=Read(engine);
        Check(Pixel(moved.get(),526,222)==blank,"drop clears chest sword icon");
        Check(Pixel(moved.get(),526,442)==blade,"drop draws equipped sword icon");
        const auto feedback=ComponentManager::GetComponentByKey(1,"state")["last_result"].cast<std::string>().value();
        Check(feedback=="moved","successful drop records player feedback: "+feedback);
        int feedback_pixels=0;
        for(int y=210;y<224;++y) for(int x=740;x<800;++x)
            if(Pixel(moved.get(),x,y)==Color{224,232,225,255})++feedback_pixels;
        Check(feedback_pixels>30,"successful drop feedback is actually rendered");
        if(argc>3) Check(IMG_SavePNG(moved.get(),argv[3]),"capture equipped inventory");
        std::cout<<"PASS inventory "<<backend<<": world visibility, bag/equipment/hotbar pixels and real mouse drop\n";return 0;
    }catch(const std::exception &error){std::cerr<<"FAIL inventory render: "<<error.what()<<'\n';return 1;}
}
