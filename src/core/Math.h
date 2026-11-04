#pragma once
#include <cmath>
#include <cstdint>
#include <random>
#include <algorithm>

namespace hk {

constexpr float kPi = 3.14159265358979f;

struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 operator/(float s) const { return {x / s, y / s}; }
    Vec2 operator-() const { return {-x, -y}; }
    Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    float len() const { return std::sqrt(x * x + y * y); }
    float len2() const { return x * x + y * y; }
    Vec2 norm() const { float l = len(); return l > 1e-6f ? Vec2{x / l, y / l} : Vec2{0, 0}; }
    Vec2 perp() const { return {-y, x}; }
    Vec2 clampLen(float m) const { float l = len(); return l > m && l > 1e-6f ? (*this) * (m / l) : *this; }
};
inline float dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline float dist(Vec2 a, Vec2 b) { return (a - b).len(); }
inline Vec2 rotate(Vec2 v, float a) { float c = std::cos(a), s = std::sin(a); return {v.x * c - v.y * s, v.x * s + v.y * c}; }

// Distance from point p to segment ab, and the projection parameter t in [0,1].
inline float distToSegment(Vec2 p, Vec2 a, Vec2 b, float* tOut = nullptr) {
    Vec2 ab = b - a;
    float l2 = ab.len2();
    float t = l2 > 1e-6f ? std::clamp(dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
    if (tOut) *tOut = t;
    return dist(p, a + ab * t);
}

inline float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

class Rng {
public:
    explicit Rng(uint64_t seed = 1) : eng_(seed) {}
    void seed(uint64_t s) { eng_.seed(s); }
    float uniform() { return std::uniform_real_distribution<float>(0.f, 1.f)(eng_); }
    float range(float a, float b) { return a + (b - a) * uniform(); }
    int irange(int a, int b) { return std::uniform_int_distribution<int>(a, b)(eng_); }  // inclusive
    float normal(float mean = 0.f, float sd = 1.f) { return std::normal_distribution<float>(mean, sd)(eng_); }
    bool chance(float p) { return uniform() < p; }
    // Probability that an event with rate `perSecond` happens within dt.
    bool rate(float perSecond, float dt) { return uniform() < 1.f - std::exp(-perSecond * dt); }
private:
    std::mt19937_64 eng_;
};

}  // namespace hk
