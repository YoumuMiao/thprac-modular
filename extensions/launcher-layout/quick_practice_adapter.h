#pragma once

#include "../quick-practice/service.h"
#include "thprac_extension.h"
#include "thprac/src/thprac/thprac_gui_components.h"
#include <cstring>
#include <type_traits>
#include <cmath>
#include <limits>
#include <tuple>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace THPrac::QuickPractice {
const Request* Preview();
bool Active();
bool Claim(const ThpracGameHost& host, const char* game);
void Install();
const Request* PendingParameters();
bool Apply(void* parameters, uint32_t size);

template <typename T>
auto& SettingValue(T& control)
{
    if constexpr (requires { *control; }) return *control;
    else return control;
}

template <typename T>
bool ReadSettingValue(const rapidjson::Value& json, T& value)
{
    if constexpr (std::is_same_v<T, bool>) {
        if (!json.IsBool()) return false;
        value = json.GetBool();
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!json.IsNumber() || !std::isfinite(json.GetDouble())
            || std::abs(json.GetDouble()) > std::numeric_limits<T>::max()) return false;
        value = static_cast<T>(json.GetDouble());
    } else {
        static_assert(std::is_integral_v<T> && std::is_signed_v<T>);
        if (!json.IsInt64() || json.GetInt64() < std::numeric_limits<T>::min()
            || json.GetInt64() > std::numeric_limits<T>::max()) return false;
        value = static_cast<T>(json.GetInt64());
    }
    return true;
}

template <typename... Controls>
int EditSettings(Edit action, Request& request, const char* fields, Controls&... controls)
{
    uint32_t schema = 2166136261u;
    for (const auto* character = fields; *character; ++character)
        schema = (schema ^ static_cast<unsigned char>(*character)) * 16777619u;

    rapidjson::Document json;
    if (action == Edit::RestoreSettings) {
        if (!request.bytes || request.bytes > sizeof(request.parameters)
            || json.Parse(reinterpret_cast<const char*>(request.parameters), request.bytes).HasParseError()
            || !json.IsObject() || !json.HasMember("schema") || !json["schema"].IsUint()
            || json["schema"].GetUint() != schema || !json.HasMember("values")
            || !json["values"].IsArray() || json["values"].Size() != sizeof...(Controls)) return 0;

        auto restored = std::tuple { SettingValue(controls)... };
        size_t index = 0;
        const auto valid = std::apply([&](auto&... values) {
            return ((ReadSettingValue(json["values"][static_cast<rapidjson::SizeType>(index++)], values)
                && [&] {
                    if constexpr (requires { controls.IsValidSelection(values); })
                        return controls.IsValidSelection(values);
                    else return true;
                }()) && ...);
        }, restored);
        if (!valid) return 0;
        std::tie(SettingValue(controls)...) = restored;
        return 1;
    }
    if (action != Edit::SnapshotSettings) return 0;

    json.SetObject();
    auto& allocator = json.GetAllocator();
    json.AddMember("schema", schema, allocator);
    rapidjson::Value values(rapidjson::kArrayType);
    (values.PushBack(rapidjson::Value(SettingValue(controls)), allocator), ...);
    json.AddMember("values", values, allocator);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    json.Accept(writer);
    if (buffer.GetSize() > sizeof(request.parameters)) return 0;
    request.bytes = static_cast<uint32_t>(buffer.GetSize());
    std::memcpy(request.parameters, buffer.GetString(), request.bytes);
    return 1;
}

template <typename Parameters>
bool UseParameters(Parameters& parameters)
{
    static_assert(std::is_trivially_copyable_v<Parameters>);
    return Apply(&parameters, sizeof(parameters));
}

class PreviewScope {
public:
    explicit PreviewScope(const EditorContext& context, const Request& request);
    ~PreviewScope();

private:
    ImGuiContext* previousContext;
    Gui::locale_t previousLocale;
    ImGuiMemAllocFunc previousAlloc;
    ImGuiMemFreeFunc previousFree;
    void* previousAllocContext;
};

template <typename Fields>
int EditMenu(auto& menu, auto& parameters, Edit action, Request& request)
{
    static_assert(std::is_trivially_copyable_v<Fields> && sizeof(Fields) <= sizeof(request.parameters));
    switch (action) {
    case Edit::RestoreSettings:
    case Edit::SnapshotSettings:
        return menu.QuickPracticeSettings(action, request);
    case Edit::Reset:
        menu.State(1);
        return 1;
    case Edit::Draw:
        menu.DrawPracticeContents();
        return 1;
    case Edit::Save:
        parameters.Reset();
        menu.State(3);
        request.stage = parameters.stage;
        request.bytes = sizeof(Fields);
        std::memcpy(request.parameters, &static_cast<const Fields&>(parameters), sizeof(Fields));
        return 1;
    default:
        return 0;
    }
}

}

#define THPRAC_PRACTICE_SETTINGS(...) \
    int QuickPracticeSettings(THPrac::QuickPractice::Edit action, THPrac::QuickPractice::Request& request) \
    { \
        return THPrac::QuickPractice::EditSettings(action, request, #__VA_ARGS__, __VA_ARGS__); \
    }
#define THPRAC_PRACTICE_EDITOR_FIELDS(fields) \
    int EditQuickPractice(THPrac::QuickPractice::Edit action, THPrac::QuickPractice::Request& request) \
    { \
        auto& menu = THGuiPrac::singleton(); \
        return THPrac::QuickPractice::EditMenu<fields>(menu, thPracParam, action, request); \
    }

#define THPRAC_PRACTICE_EDITOR() THPRAC_PRACTICE_EDITOR_FIELDS(THPracParam)
