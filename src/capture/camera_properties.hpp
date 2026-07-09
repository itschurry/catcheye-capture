#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "catcheye/input/frame_source.hpp"

namespace catcheye::capture {

enum class CameraPropertyType {
    Boolean,
    Integer,
    Float,
    Enum,
};

struct CameraPropertySpec {
    std::string_view key;
    CameraPropertyType type;
};

struct CameraPropertyValue {
    CameraPropertyType type = CameraPropertyType::Integer;
    bool bool_value = false;
    int int_value = 0;
    double float_value = 0.0;
    std::string string_value;
};

using CameraPropertyMap = std::map<std::string, CameraPropertyValue>;

const std::vector<CameraPropertySpec>& camera_property_specs();
std::optional<CameraPropertySpec> find_camera_property(std::string_view key);
CameraPropertyMap load_camera_properties_file(const std::string& path);
void save_camera_properties_file(const std::string& path, const CameraPropertyMap& properties);
bool apply_camera_properties(
    catcheye::input::FrameSource& source,
    const CameraPropertyMap& properties,
    std::string& error);
std::optional<CameraPropertyValue> parse_camera_property_value_body(
    std::string_view body,
    CameraPropertyType expected_type,
    std::string& error);

} // namespace catcheye::capture
