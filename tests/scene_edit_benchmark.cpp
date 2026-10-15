#include "core/Engine.h"
#include "shared/resources/ResourcePath.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
void Check(bool value,const std::string &message){if(!value)throw std::runtime_error(message);}
struct Fixture {
    fs::path previous=ResourcePath::ResourcesRootPath();
    fs::path root=fs::temp_directory_path()/("pocket-scene-edit-benchmark-"+std::to_string(Clock::now().time_since_epoch().count()));
    Fixture(int count,bool deep){
        fs::create_directories(root/"scenes");fs::create_directories(root/"component_types");
        std::ofstream(root/"game.config")<<R"({"initial_scene":"main"})";
        std::ofstream scene(root/"scenes/main.scene");scene<<"{\"actors\":[";
        for(int index=1;index<=count;++index){
            if(index>1)scene<<',';
            scene<<"{\"uid\":"<<index<<",\"name\":\"node"<<index<<"\",\"parent_uid\":"<<(index>1?(deep?index-1:1):0)<<",\"components\":{\"transform\":{\"type\":\"Transform\",\"x\":0.01}}}";
        }
        scene<<"]}";scene.close();ResourcePath::SetResourcesRootPath(root);
    }
    ~Fixture(){ResourcePath::SetResourcesRootPath(previous);std::error_code ignored;fs::remove_all(root,ignored);}
};
void Run(int count,bool deep,bool reparent){
    Fixture fixture(count,deep);Engine engine;engine.InitializeHeadless();
    auto *first=engine.GetRuntimeActorByUID(1);const auto generation=first->runtime_generation;
    std::vector<double> samples;
    for(int index=0;index<10;++index){
        SceneFormat::SceneMutation mutation;
        if(reparent)mutation.payload=SceneFormat::SetActorParentMutation{static_cast<Actor::UID>(count),index%2?Actor::UID(deep?count-1:1):Actor::UID(0)};
        else mutation.payload=SceneFormat::SetActorNameMutation{static_cast<Actor::UID>(count),index%2?"renamed-a":"renamed-b"};
        const auto start=Clock::now();
        Check(engine.ApplySceneEditCommand(SceneFormat::SceneEditCommand::Single(std::move(mutation))),engine.GetLastSceneEditError());
        const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        if(index>=3)samples.push_back(elapsed);
    }
    Check(engine.GetRuntimeActorByUID(1)==first&&first->runtime_generation==generation,"edit changed unrelated actor identity");
    Check(engine.GetRuntimeActors().size()==static_cast<std::size_t>(count),"edit changed actor count");
    std::sort(samples.begin(),samples.end());
    std::cout<<(deep?"chain":"flat")<<','<<(reparent?"reparent":"rename")<<','<<count<<','<<std::fixed<<std::setprecision(3)<<samples[samples.size()/2]<<','<<samples.front()<<'\n';
}
}
int main(){try{std::cout<<"shape,operation,actors,median_ms,min_ms\n";for(int count:{100,1000})for(bool deep:{false,true})for(bool reparent:{false,true})Run(count,deep,reparent);return 0;}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}
