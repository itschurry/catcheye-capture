#include "capture/camera_properties.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace catcheye::capture {
namespace {

class JsonParser {
  public:
    explicit JsonParser(std::string_view text)
        : text_(text) {}

    CameraPropertyMap parse_object()
    {
        skip_utf8_bom();
        skip_ws();
        expect('{');
        CameraPropertyMap values;
        skip_ws();
        if (consume('}')) {
            return values;
        }

        while (true) {
            const std::string key = parse_string();
            skip_ws();
            expect(':');
            values[key] = parse_value();
            skip_ws();
            if (consume('}')) {
                break;
            }
            expect(',');
        }
        skip_ws();
        if (pos_ != text_.size()) {
            throw std::runtime_error("unexpected trailing JSON content");
        }
        return values;
    }

  private:
    void skip_utf8_bom()
    {
        if (text_.size() >= 3U &&
            static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            pos_ = 3U;
        }
    }

    void skip_ws()
    {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool consume(char expected)
    {
        if (pos_ < text_.size() && text_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char expected)
    {
        skip_ws();
        if (!consume(expected)) {
            throw std::runtime_error(std::string("expected JSON token '") + expected + "'");
        }
        skip_ws();
    }

    std::string parse_string()
    {
        skip_ws();
        if (!consume('"')) {
            throw std::runtime_error("expected JSON string");
        }

        std::string value;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') {
                return value;
            }
            if (ch != '\\') {
                value.push_back(ch);
                continue;
            }
            if (pos_ >= text_.size()) {
                throw std::runtime_error("invalid JSON string escape");
            }
            const char escaped = text_[pos_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(escaped);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    throw std::runtime_error("unsupported JSON string escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }

    CameraPropertyValue parse_value()
    {
        skip_ws();
        if (pos_ >= text_.size()) {
            throw std::runtime_error("expected JSON value");
        }
        if (text_.substr(pos_, 4) == "true") {
            pos_ += 4;
            CameraPropertyValue value;
            value.type = CameraPropertyType::Boolean;
            value.bool_value = true;
            return value;
        }
        if (text_.substr(pos_, 5) == "false") {
            pos_ += 5;
            CameraPropertyValue value;
            value.type = CameraPropertyType::Boolean;
            value.bool_value = false;
            return value;
        }
        if (text_[pos_] == '"') {
            CameraPropertyValue value;
            value.type = CameraPropertyType::Enum;
            value.string_value = parse_string();
            return value;
        }
        if (text_[pos_] == '-' || std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
            return parse_number();
        }
        throw std::runtime_error("unsupported JSON value");
    }

    CameraPropertyValue parse_number()
    {
        const std::size_t start = pos_;
        if (text_[pos_] == '-') {
            ++pos_;
        }
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
        bool is_float = false;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            is_float = true;
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            is_float = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
                ++pos_;
            }
        }

        const std::string token(text_.substr(start, pos_ - start));
        CameraPropertyValue value;
        if (is_float) {
            value.type = CameraPropertyType::Float;
            value.float_value = std::stod(token);
            if (!std::isfinite(value.float_value)) {
                throw std::runtime_error("JSON number must be finite");
            }
            return value;
        }

        const long long parsed = std::stoll(token);
        if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
            throw std::runtime_error("JSON integer is out of range");
        }
        value.type = CameraPropertyType::Integer;
        value.int_value = static_cast<int>(parsed);
        value.float_value = static_cast<double>(value.int_value);
        return value;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

std::string json_string(std::string_view value)
{
    std::ostringstream oss;
    oss << '"';
    for (const char ch : value) {
        switch (ch) {
            case '"':
            case '\\':
                oss << '\\' << ch;
                break;
            case '\b':
                oss << "\\b";
                break;
            case '\f':
                oss << "\\f";
                break;
            case '\n':
                oss << "\\n";
                break;
            case '\r':
                oss << "\\r";
                break;
            case '\t':
                oss << "\\t";
                break;
            default:
                oss << ch;
                break;
        }
    }
    oss << '"';
    return oss.str();
}

std::string first_bytes_hex(std::string_view text)
{
    std::ostringstream oss;
    const std::size_t count = std::min<std::size_t>(text.size(), 8U);
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0U) {
            oss << ' ';
        }
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(static_cast<unsigned char>(text[i]));
    }
    return oss.str();
}

std::string value_to_json(const CameraPropertyValue& value)
{
    switch (value.type) {
        case CameraPropertyType::Boolean:
            return value.bool_value ? "true" : "false";
        case CameraPropertyType::Integer:
            return std::to_string(value.int_value);
        case CameraPropertyType::Float: {
            std::ostringstream oss;
            oss << value.float_value;
            return oss.str();
        }
        case CameraPropertyType::Enum:
            return json_string(value.string_value);
    }
    return "null";
}

bool value_matches_type(const CameraPropertyValue& value, CameraPropertyType expected)
{
    if (expected == CameraPropertyType::Float) {
        return value.type == CameraPropertyType::Float || value.type == CameraPropertyType::Integer;
    }
    return value.type == expected;
}

std::string type_error(CameraPropertyType expected)
{
    switch (expected) {
        case CameraPropertyType::Boolean:
            return "property value must be boolean";
        case CameraPropertyType::Integer:
            return "property value must be integer";
        case CameraPropertyType::Float:
            return "property value must be number";
        case CameraPropertyType::Enum:
            return "property value must be string";
    }
    return "invalid property value type";
}

void validate_property(std::string_view key, const CameraPropertyValue& value)
{
    const auto spec = find_camera_property(key);
    if (!spec.has_value()) {
        throw std::runtime_error("unsupported RGB camera property: " + std::string(key));
    }
    if (!value_matches_type(value, spec->type)) {
        throw std::runtime_error(type_error(spec->type) + ": " + std::string(key));
    }
}

} // namespace

const std::vector<CameraPropertySpec>& camera_property_specs()
{
    static const std::vector<CameraPropertySpec> specs = {
        {"ae-enable", CameraPropertyType::Boolean},
        {"ae-metering-mode", CameraPropertyType::Enum},
        {"ae-flicker-period", CameraPropertyType::Integer},
        {"exposure-time-mode", CameraPropertyType::Enum},
        {"exposure-time", CameraPropertyType::Integer},
        {"exposure-value", CameraPropertyType::Float},
        {"analogue-gain-mode", CameraPropertyType::Enum},
        {"analogue-gain", CameraPropertyType::Float},
        {"awb-enable", CameraPropertyType::Boolean},
        {"awb-mode", CameraPropertyType::Enum},
        {"af-mode", CameraPropertyType::Enum},
        {"lens-position", CameraPropertyType::Float},
        {"brightness", CameraPropertyType::Float},
        {"contrast", CameraPropertyType::Float},
        {"saturation", CameraPropertyType::Float},
        {"sharpness", CameraPropertyType::Float},
        {"gamma", CameraPropertyType::Float},
    };
    return specs;
}

std::optional<CameraPropertySpec> find_camera_property(std::string_view key)
{
    for (const auto& spec : camera_property_specs()) {
        if (spec.key == key) {
            return spec;
        }
    }
    return std::nullopt;
}

CameraPropertyMap load_camera_properties_file(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open RGB camera properties config: " + path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();
    CameraPropertyMap properties;
    try {
        JsonParser parser(text);
        properties = parser.parse_object();
    } catch (const std::exception& exception) {
        throw std::runtime_error(
            "failed to parse RGB camera properties config '" + path + "': " +
            exception.what() + " (first bytes: " + first_bytes_hex(text) + ")");
    }
    for (const auto& [key, value] : properties) {
        validate_property(key, value);
    }
    return properties;
}

void save_camera_properties_file(const std::string& path, const CameraPropertyMap& properties)
{
    for (const auto& [key, value] : properties) {
        validate_property(key, value);
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to write RGB camera properties config: " + path);
    }
    if (properties.empty()) {
        output << "{}\n";
        if (!output) {
            throw std::runtime_error("failed to flush RGB camera properties config: " + path);
        }
        return;
    }

    output << "{\n";
    bool first = true;
    for (const auto& spec : camera_property_specs()) {
        const auto it = properties.find(std::string(spec.key));
        if (it == properties.end()) {
            continue;
        }
        if (!first) {
            output << ",\n";
        }
        first = false;
        output << "  " << json_string(spec.key) << ": " << value_to_json(it->second);
    }
    output << "\n}\n";
    if (!output) {
        throw std::runtime_error("failed to flush RGB camera properties config: " + path);
    }
}

bool apply_camera_properties(
    catcheye::input::FrameSource& source,
    const CameraPropertyMap& properties,
    std::string& error)
{
    for (const auto& spec : camera_property_specs()) {
        const auto it = properties.find(std::string(spec.key));
        if (it == properties.end()) {
            continue;
        }
        const auto& value = it->second;
        bool updated = false;
        switch (spec.type) {
            case CameraPropertyType::Boolean:
                updated = source.set_bool_property(spec.key, value.bool_value);
                break;
            case CameraPropertyType::Integer:
                updated = source.set_int_property(spec.key, value.int_value);
                break;
            case CameraPropertyType::Float:
                updated = source.set_float_property(
                    spec.key,
                    static_cast<float>(value.type == CameraPropertyType::Integer ? value.int_value : value.float_value));
                break;
            case CameraPropertyType::Enum:
                updated = source.set_string_property(spec.key, value.string_value);
                break;
        }
        if (!updated) {
            error = "failed to set RGB camera property: " + std::string(spec.key);
            return false;
        }
    }
    return true;
}

std::optional<CameraPropertyValue> parse_camera_property_value_body(
    std::string_view body,
    CameraPropertyType expected_type,
    std::string& error)
{
    try {
        JsonParser parser(body);
        CameraPropertyMap values = parser.parse_object();
        if (values.size() != 1U || values.find("value") == values.end()) {
            error = "invalid property JSON body";
            return std::nullopt;
        }
        CameraPropertyValue value = values.at("value");
        if (!value_matches_type(value, expected_type)) {
            error = type_error(expected_type);
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        error = "invalid property JSON body";
        return std::nullopt;
    }
}

} // namespace catcheye::capture
