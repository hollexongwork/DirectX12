#pragma once

#include <DirectXMath.h>
#include <cmath>

using namespace DirectX;

inline XMFLOAT3 operator-(const XMFLOAT3& v)
{
    return XMFLOAT3(-v.x, -v.y, -v.z);
}

// + -
inline XMFLOAT3 operator+(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return XMFLOAT3(a.x + b.x, a.y + b.y, a.z + b.z);
}

inline XMFLOAT3 operator-(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return XMFLOAT3(a.x - b.x, a.y - b.y, a.z - b.z);
}

inline XMFLOAT3& operator+=(XMFLOAT3& a, const XMFLOAT3& b)
{
    a.x += b.x; a.y += b.y; a.z += b.z;
    return a;
}

inline XMFLOAT3& operator-=(XMFLOAT3& a, const XMFLOAT3& b)
{
    a.x -= b.x; a.y -= b.y; a.z -= b.z;
    return a;
}

// x /
inline XMFLOAT3 operator*(const XMFLOAT3& v, float s)
{
    return XMFLOAT3(v.x * s, v.y * s, v.z * s);
}

inline XMFLOAT3 operator*(float s, const XMFLOAT3& v)
{
    return v * s;
}

inline XMFLOAT3 operator/(const XMFLOAT3& v, float s)
{
    const float inv = 1.0f / s;
    return XMFLOAT3(v.x * inv, v.y * inv, v.z * inv);
}

inline XMFLOAT3& operator*=(XMFLOAT3& v, float s)
{
    v.x *= s; v.y *= s; v.z *= s;
    return v;
}

inline XMFLOAT3& operator/=(XMFLOAT3& v, float s)
{
    const float inv = 1.0f / s;
    v.x *= inv; v.y *= inv; v.z *= inv;
    return v;
}

inline XMFLOAT3 operator*(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return XMFLOAT3(a.x * b.x, a.y * b.y, a.z * b.z);
}

inline XMFLOAT3 operator/(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return XMFLOAT3(a.x / b.x, a.y / b.y, a.z / b.z);
}

// == !=
inline bool operator==(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return (a.x == b.x) && (a.y == b.y) && (a.z == b.z);
}

inline bool operator!=(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return !(a == b);
}