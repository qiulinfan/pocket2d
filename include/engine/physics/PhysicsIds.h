#ifndef PHYSICS_IDS_H
#define PHYSICS_IDS_H

#include "box2d/box2d.h"

// Box2D ids are value handles; equal ids name the same object. Body and shape
// ids hold no world generation, so check their world before trusting them.
inline bool operator==(b2WorldId a, b2WorldId b) {
    return a.index1 == b.index1 && a.generation == b.generation;
}
inline bool operator==(b2BodyId a, b2BodyId b) { return B2_ID_EQUALS(a, b); }
inline bool operator==(b2ShapeId a, b2ShapeId b) { return B2_ID_EQUALS(a, b); }

#endif
