#ifndef POCKET2D_RUNTIME_ACTOR_PLAN_H
#define POCKET2D_RUNTIME_ACTOR_PLAN_H

#include "scene/Actor.h"
#include "scripting/RuntimeActorAssets.h"
#include <list>
#include <memory>
#include <string>
#include <vector>

struct lua_State;
class PreparedRuntimeActor {
public:
    ~PreparedRuntimeActor();
    PreparedRuntimeActor(PreparedRuntimeActor &&) noexcept;
    PreparedRuntimeActor &operator=(PreparedRuntimeActor &&) noexcept;
    PreparedRuntimeActor(const PreparedRuntimeActor &) = delete;
    PreparedRuntimeActor &operator=(const PreparedRuntimeActor &) = delete;
    bool CommitStarted() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
    explicit PreparedRuntimeActor(lua_State *state);
    friend std::unique_ptr<PreparedRuntimeActor> PrepareRuntimeActor(Actor candidate, std::string &error);
    friend Actor *PublishRuntimeActor(PreparedRuntimeActor &plan, std::list<Actor> &actors, bool deferred_activation);
};

// Owner-thread token; destroy before VM shutdown. UID reservations may have
// gaps. Rejected preparation never registers actors or calls gameplay teardown.
std::unique_ptr<PreparedRuntimeActor> PrepareRuntimeActor(Actor candidate, std::string &error);
// After CommitStarted becomes true, unexpected allocation/install errors are a
// host fatal/reset boundary, not a recoverable rollback guarantee.
Actor *PublishRuntimeActor(PreparedRuntimeActor &plan, std::list<Actor> &actors, bool deferred_activation);

#endif
