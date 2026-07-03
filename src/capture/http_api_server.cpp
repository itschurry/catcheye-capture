#include "capture/http_api_server.hpp"

#include <cctype>
#include <cmath>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include "catcheye/utils/logger.hpp"
#include "capture/processor.hpp"
#include "capture/recording_controller.hpp"

namespace catcheye::capture {
namespace {

struct JsonValue {
    enum class Type {
        Boolean,
        Integer,
        Float,
        String,
    };

    Type type = Type::Integer;
    bool bool_value = false;
    int int_value = 0;
    float float_value = 0.0F;
    std::string string_value;
};

enum class RuntimePropertyType {
    Boolean,
    Integer,
    Float,
    Enum,
};

struct RuntimePropertySpec {
    std::string_view key;
    RuntimePropertyType type;
};

constexpr RuntimePropertySpec RGB_CAMERA_PROPERTIES[] = {
    {"ae-enable", RuntimePropertyType::Boolean},
    {"ae-metering-mode", RuntimePropertyType::Enum},
    {"ae-flicker-period", RuntimePropertyType::Integer},
    {"exposure-time-mode", RuntimePropertyType::Enum},
    {"exposure-time", RuntimePropertyType::Integer},
    {"exposure-value", RuntimePropertyType::Float},
    {"analogue-gain-mode", RuntimePropertyType::Enum},
    {"analogue-gain", RuntimePropertyType::Float},
    {"awb-enable", RuntimePropertyType::Boolean},
    {"awb-mode", RuntimePropertyType::Enum},
    {"af-mode", RuntimePropertyType::Enum},
    {"lens-position", RuntimePropertyType::Float},
    {"brightness", RuntimePropertyType::Float},
    {"contrast", RuntimePropertyType::Float},
    {"saturation", RuntimePropertyType::Float},
    {"sharpness", RuntimePropertyType::Float},
    {"gamma", RuntimePropertyType::Float},
};

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::optional<RuntimePropertySpec> find_rgb_camera_property(std::string_view key)
{
    for (const auto& spec : RGB_CAMERA_PROPERTIES) {
        if (spec.key == key) {
            return spec;
        }
    }
    return std::nullopt;
}

bool parse_value_body(std::string_view body, JsonValue& output)
{
    const std::size_t key_pos = body.find("\"value\"");
    if (key_pos == std::string_view::npos) {
        return false;
    }
    const std::size_t colon_pos = body.find(':', key_pos);
    if (colon_pos == std::string_view::npos) {
        return false;
    }

    std::string value_text = trim(std::string(body.substr(colon_pos + 1U)));
    if (!value_text.empty() && value_text.back() == '}') {
        value_text.pop_back();
    }
    value_text = trim(value_text);
    if (value_text == "true" || value_text == "false") {
        output.type = JsonValue::Type::Boolean;
        output.bool_value = value_text == "true";
        return true;
    }
    if (value_text.size() >= 2U && value_text.front() == '"' && value_text.back() == '"') {
        output.type = JsonValue::Type::String;
        output.string_value = value_text.substr(1U, value_text.size() - 2U);
        return true;
    }

    try {
        std::size_t consumed = 0;
        const int value = std::stoi(value_text, &consumed);
        if (consumed == value_text.size()) {
            output.type = JsonValue::Type::Integer;
            output.int_value = value;
            return true;
        }
    } catch (...) {
    }

    try {
        std::size_t consumed = 0;
        const float value = std::stof(value_text, &consumed);
        if (consumed != value_text.size() || !std::isfinite(value)) {
            return false;
        }
        output.type = JsonValue::Type::Float;
        output.float_value = value;
        return true;
    } catch (...) {
        return false;
    }
}

catcheye::http::HttpResponse get_rgb_camera_properties(catcheye::input::FrameSource* camera_source)
{
    if (camera_source == nullptr) {
        return {409, "Conflict", catcheye::http::json_error_body("RGB camera is not enabled")};
    }

    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& spec : RGB_CAMERA_PROPERTIES) {
        const auto value = camera_source->property_json(spec.key);
        if (!value.has_value()) {
            continue;
        }
        if (!first) {
            oss << ',';
        }
        first = false;
        oss << '"' << spec.key << "\":" << *value;
    }
    oss << "}";
    return {200, "OK", oss.str()};
}

catcheye::http::HttpResponse put_rgb_camera_property(
    catcheye::input::FrameSource* camera_source,
    const std::string& key,
    const std::string& body)
{
    if (camera_source == nullptr) {
        return {409, "Conflict", catcheye::http::json_error_body("RGB camera is not enabled")};
    }
    const auto spec = find_rgb_camera_property(key);
    if (!spec.has_value()) {
        return {400, "Bad Request", catcheye::http::json_error_body("unsupported RGB camera property")};
    }

    JsonValue value;
    if (!parse_value_body(body, value)) {
        return {400, "Bad Request", catcheye::http::json_error_body("invalid property JSON body")};
    }

    bool updated = false;
    switch (spec->type) {
        case RuntimePropertyType::Boolean:
            if (value.type != JsonValue::Type::Boolean) {
                return {400, "Bad Request", catcheye::http::json_error_body("property value must be boolean")};
            }
            updated = camera_source->set_bool_property(key, value.bool_value);
            break;
        case RuntimePropertyType::Integer:
            if (value.type != JsonValue::Type::Integer) {
                return {400, "Bad Request", catcheye::http::json_error_body("property value must be integer")};
            }
            updated = camera_source->set_int_property(key, value.int_value);
            break;
        case RuntimePropertyType::Float:
            if (value.type != JsonValue::Type::Float && value.type != JsonValue::Type::Integer) {
                return {400, "Bad Request", catcheye::http::json_error_body("property value must be number")};
            }
            updated = camera_source->set_float_property(
                key,
                value.type == JsonValue::Type::Float ? value.float_value : static_cast<float>(value.int_value));
            break;
        case RuntimePropertyType::Enum:
            if (value.type != JsonValue::Type::String) {
                return {400, "Bad Request", catcheye::http::json_error_body("property value must be string")};
            }
            updated = camera_source->set_string_property(key, value.string_value);
            break;
    }

    if (!updated) {
        return {500, "Internal Server Error", catcheye::http::json_error_body("failed to set RGB camera property")};
    }
    return get_rgb_camera_properties(camera_source);
}

catcheye::http::HttpResponse recording_response(const RecordingStatus& status)
{
    return {200, "OK", recording_status_json(status)};
}

} // namespace

HttpApiServer::HttpApiServer(
    HttpApiServerConfig config,
    CaptureProcessor* processor,
    catcheye::input::FrameSource* camera_source)
    : config_(std::move(config)),
      processor_(processor),
      camera_source_(camera_source)
{
}

HttpApiServer::~HttpApiServer()
{
    stop();
}

bool HttpApiServer::start()
{
    if (server_ != nullptr) {
        return true;
    }
    if (processor_ == nullptr || config_.port <= 0) {
        return false;
    }

    server_ = std::make_unique<catcheye::http::HttpServer>(catcheye::http::HttpServerConfig{
        .bind_address = config_.bind_address,
        .port = config_.port,
    });

    server_->add_route("/api/device-info", [](const catcheye::http::HttpRequest& request) {
        if (request.method == "GET") {
            return catcheye::http::HttpResponse{200, "OK", R"({"app":"catcheye-capture","kind":"capture"})"};
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_route("/api/capture/status", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "GET") {
            return catcheye::http::HttpResponse{200, "OK", processor_->status_json()};
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_route("/api/capture/request", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "POST") {
            processor_->request_capture();
            return catcheye::http::HttpResponse{200, "OK", processor_->status_json()};
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_route("/api/rgb-camera/properties", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "GET") {
            return get_rgb_camera_properties(camera_source_);
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    constexpr std::string_view rgb_camera_property_prefix = "/api/rgb-camera/properties/";
    constexpr std::size_t rgb_camera_property_prefix_size = rgb_camera_property_prefix.size();
    server_->add_prefix_route(std::string(rgb_camera_property_prefix), [this, rgb_camera_property_prefix_size](const catcheye::http::HttpRequest& request) {
        const std::string key = request.path.substr(rgb_camera_property_prefix_size);
        if (request.method == "PUT") {
            return put_rgb_camera_property(camera_source_, key, request.body);
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_route("/api/recording", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "GET") {
            return recording_response(processor_->recording_status());
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_route("/api/recording/start", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "POST") {
            return recording_response(processor_->start_recording());
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_route("/api/recording/pause", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "POST") {
            return recording_response(processor_->pause_recording());
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_route("/api/recording/resume", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "POST") {
            return recording_response(processor_->resume_recording());
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_route("/api/recording/save", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "POST") {
            return recording_response(processor_->save_recording());
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_route("/api/recording/cancel", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "POST") {
            return recording_response(processor_->cancel_recording());
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    if (!server_->start()) {
        server_.reset();
        return false;
    }

    if (const auto log = logger()) {
        log->info("HTTP API listening on {}:{}", config_.bind_address, config_.port);
    }
    return true;
}

void HttpApiServer::stop()
{
    if (server_ != nullptr) {
        server_->stop();
        server_.reset();
    }
}

} // namespace catcheye::capture
