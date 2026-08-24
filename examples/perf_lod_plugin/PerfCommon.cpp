// UEVR perf_lod_plugin - shared helpers.
// Licensed under the MIT license, separate from the rest of the UEVR codebase.
#include <vector>

#include <Windows.h>

#include "PerfCommon.hpp"

using namespace uevr;

namespace perflod {
API::FProperty* find_property_recursive(API::UStruct* strct, const wchar_t* name) {
    if (strct == nullptr || name == nullptr) {
        return nullptr;
    }

    for (auto super = strct; super != nullptr; super = super->get_super()) {
        if (const auto prop = super->find_property(name); prop != nullptr) {
            return prop;
        }
    }

    return nullptr;
}

API::UFunction* find_function_recursive(API::UStruct* strct, const wchar_t* name) {
    if (strct == nullptr || name == nullptr) {
        return nullptr;
    }

    for (auto super = strct; super != nullptr; super = super->get_super()) {
        if (const auto fn = super->find_function(name); fn != nullptr) {
            return fn;
        }
    }

    return nullptr;
}

std::wstring property_class_name(API::FProperty* prop) {
    if (prop == nullptr) {
        return L"";
    }

    const auto field_class = prop->get_class();

    if (field_class == nullptr) {
        return L"";
    }

    const auto fname = field_class->get_fname();

    if (fname == nullptr) {
        return L"";
    }

    return fname->to_string();
}

std::string narrow(const std::wstring& str) {
    if (str.empty()) {
        return "";
    }

    const auto size = WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0, nullptr, nullptr);

    if (size <= 0) {
        return "";
    }

    std::string result((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size, nullptr, nullptr);

    return result;
}
} // namespace perflod
