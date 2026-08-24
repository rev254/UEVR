// UEVR perf_lod_plugin - shared helpers.
// Licensed under the MIT license, separate from the rest of the UEVR codebase.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include "uevr/API.hpp"

namespace perflod {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg2Rad = kPi / 180.0;

struct Vec3 {
    double x{};
    double y{};
    double z{};

    Vec3 operator-(const Vec3& o) const { return Vec3{x - o.x, y - o.y, z - o.z}; }
    double length_sq() const { return (x * x) + (y * y) + (z * z); }
    double length() const { return std::sqrt(length_sq()); }
};

inline double dot(const Vec3& a, const Vec3& b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

inline Vec3 normalize(const Vec3& v) {
    const auto len = v.length();

    if (len <= 1e-6) {
        return Vec3{0.0, 0.0, 0.0};
    }

    return Vec3{v.x / len, v.y / len, v.z / len};
}

// UE FRotator (degrees; X forward, Y right, Z up) -> unit forward vector.
// Matches FRotator::Vector(): (CP*CY, CP*SY, SP).
inline Vec3 rotator_to_forward(double pitch_deg, double yaw_deg) {
    const auto pitch = pitch_deg * kDeg2Rad;
    const auto yaw = yaw_deg * kDeg2Rad;
    const auto cos_pitch = std::cos(pitch);

    return Vec3{cos_pitch * std::cos(yaw), cos_pitch * std::sin(yaw), std::sin(pitch)};
}

// Walks the whole class/struct hierarchy. UEVR's find_property does not
// guarantee it searches super structs, so never rely on it alone.
uevr::API::FProperty* find_property_recursive(uevr::API::UStruct* strct, const wchar_t* name);
uevr::API::UFunction* find_function_recursive(uevr::API::UStruct* strct, const wchar_t* name);

// Name of the property's own FFieldClass, e.g. L"BoolProperty", L"FloatProperty".
std::wstring property_class_name(uevr::API::FProperty* prop);

std::string narrow(const std::wstring& str);
} // namespace perflod
