#ifndef COLLISION_H
#define COLLISION_H

#include "core/Vector2.h"
#include "scene/ActorHandle.h"

struct Actor;

struct Collision {
    // other actor involved in this contact
    // 与当前 actor 接触的另一个 actor
    Actor *other = nullptr;
    ActorHandle other_handle;

    // world-space contact point. trigger events may use a sentinel value
    // 世界坐标下的接触点. trigger 事件可能会用哨兵值
    Vector2 point = Vector2(0.0f, 0.0f);

    // relative velocity at the contact
    // 接触时的相对速度
    Vector2 relative_velocity = Vector2(0.0f, 0.0f);

    // contact normal. trigger events may not have a real normal
    // 接触法线. trigger 事件可能没有真实法线
    Vector2 normal = Vector2(0.0f, 0.0f);
};

#endif
