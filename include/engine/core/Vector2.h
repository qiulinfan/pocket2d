#ifndef ENGINE_CORE_VECTOR2_H
#define ENGINE_CORE_VECTOR2_H

#include <cfloat>
#include <cmath>

// The engine's 2D vector, exposed to Lua as Vector2. Physics libraries convert
// at their boundary. Arithmetic matches the Box2D 2.4 vector the Lua API was
// first built on: float math, and Normalize leaves vectors shorter than
// FLT_EPSILON unchanged and returns 0.
struct Vector2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vector2() = default;
    constexpr Vector2(float x_in, float y_in) : x(x_in), y(y_in) {}

    void SetZero() { x = 0.0f; y = 0.0f; }
    void Set(float x_in, float y_in) { x = x_in; y = y_in; }

    Vector2 operator-() const { return {-x, -y}; }
    void operator+=(const Vector2 &v) { x += v.x; y += v.y; }
    void operator-=(const Vector2 &v) { x -= v.x; y -= v.y; }
    void operator*=(float a) { x *= a; y *= a; }

    float Length() const { return std::sqrt(x * x + y * y); }
    float LengthSquared() const { return x * x + y * y; }

    // Converts to a unit vector and returns the original length.
    float Normalize() {
        const float length = Length();
        if (length < FLT_EPSILON) return 0.0f;
        const float inverse = 1.0f / length;
        x *= inverse;
        y *= inverse;
        return length;
    }

    bool IsValid() const { return std::isfinite(x) && std::isfinite(y); }
};

inline Vector2 operator+(const Vector2 &a, const Vector2 &b) { return {a.x + b.x, a.y + b.y}; }
inline Vector2 operator-(const Vector2 &a, const Vector2 &b) { return {a.x - b.x, a.y - b.y}; }
inline Vector2 operator*(float s, const Vector2 &a) { return {s * a.x, s * a.y}; }
inline bool operator==(const Vector2 &a, const Vector2 &b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(const Vector2 &a, const Vector2 &b) { return !(a == b); }

inline float Dot(const Vector2 &a, const Vector2 &b) { return a.x * b.x + a.y * b.y; }
inline float Distance(const Vector2 &a, const Vector2 &b) { return (a - b).Length(); }

#endif
