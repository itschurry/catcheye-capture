#include "capture/http_api_server.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "catcheye/http/http_server.hpp"
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

struct CaptureImageMetadata {
    std::filesystem::path path;
    std::string date;
    std::string filename;
    std::string captured_at;
    std::uint64_t sequence = 0;
    std::uintmax_t size_bytes = 0;
    int width = 0;
    int height = 0;
};

struct CaptureStorageMetadata {
    std::string path;
    std::uintmax_t total_bytes = 0;
    std::uintmax_t available_bytes = 0;
    std::uintmax_t used_bytes = 0;
    double used_percent = 0.0;
    std::uintmax_t capture_bytes = 0;
    std::uint64_t capture_count = 0;
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

bool all_digits(std::string_view value)
{
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

bool is_valid_capture_date(std::string_view value)
{
    return value.size() == 10U &&
        value[4] == '-' &&
        value[7] == '-' &&
        all_digits(value.substr(0, 4U)) &&
        all_digits(value.substr(5U, 2U)) &&
        all_digits(value.substr(8U, 2U));
}

std::string local_timezone_offset(std::tm local_tm)
{
    local_tm.tm_isdst = -1;
    const std::time_t local_time = std::mktime(&local_tm);
    std::tm utc_tm{};
    gmtime_r(&local_time, &utc_tm);
    const std::time_t utc_as_local = std::mktime(&utc_tm);
    long offset_seconds = static_cast<long>(std::difftime(local_time, utc_as_local));
    const char sign = offset_seconds >= 0 ? '+' : '-';
    if (offset_seconds < 0) {
        offset_seconds = -offset_seconds;
    }
    const long hours = offset_seconds / 3600;
    const long minutes = (offset_seconds % 3600) / 60;

    std::ostringstream oss;
    oss << sign << std::setw(2) << std::setfill('0') << hours
        << ':' << std::setw(2) << std::setfill('0') << minutes;
    return oss.str();
}

bool parse_capture_filename(
    std::string_view date,
    std::string_view filename,
    std::uint64_t& sequence,
    std::string& captured_at)
{
    if (!is_valid_capture_date(date)) {
        return false;
    }
    if (filename.size() != 21U || filename.substr(17U) != ".jpg") {
        return false;
    }
    const std::string_view stem(filename.data(), 17U);
    if (stem[6] != '_' || stem[10] != '_') {
        return false;
    }
    const std::string_view hhmmss(stem.data(), 6U);
    const std::string_view millis(stem.data() + 7U, 3U);
    const std::string_view suffix(stem.data() + 11U, 6U);
    if (!all_digits(hhmmss) || !all_digits(millis) || !all_digits(suffix)) {
        return false;
    }

    std::uint64_t parsed = 0;
    for (const char c : suffix) {
        parsed = (parsed * 10U) + static_cast<std::uint64_t>(c - '0');
    }
    sequence = parsed;

    std::tm local_tm{};
    local_tm.tm_year = std::stoi(std::string(date.substr(0, 4U))) - 1900;
    local_tm.tm_mon = std::stoi(std::string(date.substr(5U, 2U))) - 1;
    local_tm.tm_mday = std::stoi(std::string(date.substr(8U, 2U)));
    local_tm.tm_hour = std::stoi(std::string(hhmmss.substr(0, 2U)));
    local_tm.tm_min = std::stoi(std::string(hhmmss.substr(2U, 2U)));
    local_tm.tm_sec = std::stoi(std::string(hhmmss.substr(4U, 2U)));

    std::ostringstream oss;
    oss << date << 'T'
        << hhmmss.substr(0, 2U) << ':'
        << hhmmss.substr(2U, 2U) << ':'
        << hhmmss.substr(4U, 2U) << '.'
        << millis
        << local_timezone_offset(local_tm);
    captured_at = oss.str();
    return true;
}

bool is_valid_capture_filename(std::string_view date, std::string_view filename)
{
    std::uint64_t sequence = 0;
    std::string captured_at;
    return parse_capture_filename(date, filename, sequence, captured_at);
}

std::pair<std::string, std::string> split_path_query(std::string_view path)
{
    const std::size_t query_pos = path.find('?');
    if (query_pos == std::string_view::npos) {
        return {std::string(path), ""};
    }
    return {std::string(path.substr(0, query_pos)), std::string(path.substr(query_pos + 1U))};
}

std::map<std::string, std::string> parse_query(std::string_view query)
{
    std::map<std::string, std::string> values;
    std::size_t pos = 0;
    while (pos < query.size()) {
        const std::size_t amp = query.find('&', pos);
        const std::string_view token = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        const std::size_t eq = token.find('=');
        if (eq != std::string_view::npos) {
            values[std::string(token.substr(0, eq))] = std::string(token.substr(eq + 1U));
        }
        if (amp == std::string_view::npos) {
            break;
        }
        pos = amp + 1U;
    }
    return values;
}

bool path_is_inside(const std::filesystem::path& root, const std::filesystem::path& path)
{
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const auto canonical_path = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return false;
    }
    auto root_it = canonical_root.begin();
    auto path_it = canonical_path.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++path_it) {
        if (path_it == canonical_path.end() || *root_it != *path_it) {
            return false;
        }
    }
    return true;
}

std::optional<CaptureImageMetadata> capture_image_metadata(
    const std::filesystem::path& capture_root,
    const std::string& date,
    const std::filesystem::directory_entry& entry)
{
    if (!entry.is_regular_file()) {
        return std::nullopt;
    }

    const std::string filename = entry.path().filename().string();
    std::uint64_t sequence = 0;
    std::string captured_at;
    if (!parse_capture_filename(date, filename, sequence, captured_at)) {
        return std::nullopt;
    }
    if (!path_is_inside(capture_root, entry.path())) {
        return std::nullopt;
    }

    const auto image = cv::imread(entry.path().string(), cv::IMREAD_UNCHANGED);
    CaptureImageMetadata metadata;
    metadata.path = entry.path();
    metadata.date = date;
    metadata.filename = filename;
    metadata.captured_at = captured_at;
    metadata.sequence = sequence;
    metadata.size_bytes = entry.file_size();
    metadata.width = image.empty() ? 0 : image.cols;
    metadata.height = image.empty() ? 0 : image.rows;
    return metadata;
}

std::string capture_image_metadata_json(const CaptureImageMetadata& metadata)
{
    std::ostringstream oss;
    oss << "{\"filename\":\"" << catcheye::http::escape_json_string(metadata.filename) << "\""
        << ",\"date\":\"" << catcheye::http::escape_json_string(metadata.date) << "\""
        << ",\"captured_at\":\"" << catcheye::http::escape_json_string(metadata.captured_at) << "\""
        << ",\"sequence\":" << metadata.sequence
        << ",\"size_bytes\":" << metadata.size_bytes
        << ",\"width\":" << metadata.width
        << ",\"height\":" << metadata.height
        << ",\"url\":\"/api/captures/file/"
        << catcheye::http::escape_json_string(metadata.date) << '/'
        << catcheye::http::escape_json_string(metadata.filename) << "\"}";
    return oss.str();
}

std::vector<CaptureImageMetadata> list_capture_images(const std::filesystem::path& capture_root, const std::string& date)
{
    const std::filesystem::path date_dir = capture_root / date;
    if (!std::filesystem::exists(date_dir)) {
        return {};
    }

    std::vector<CaptureImageMetadata> images;
    for (const auto& entry : std::filesystem::directory_iterator(date_dir)) {
        auto metadata = capture_image_metadata(capture_root, date, entry);
        if (metadata.has_value()) {
            images.push_back(std::move(*metadata));
        }
    }
    std::sort(images.begin(), images.end(), [](const auto& left, const auto& right) {
        return left.filename > right.filename;
    });
    return images;
}

std::string capture_storage_json(const CaptureStorageMetadata& storage)
{
    std::ostringstream oss;
    oss << "{\"path\":\"" << catcheye::http::escape_json_string(storage.path) << "\""
        << ",\"total_bytes\":" << storage.total_bytes
        << ",\"available_bytes\":" << storage.available_bytes
        << ",\"used_bytes\":" << storage.used_bytes
        << ",\"used_percent\":" << std::fixed << std::setprecision(1) << storage.used_percent
        << ",\"capture_bytes\":" << storage.capture_bytes
        << ",\"capture_count\":" << storage.capture_count
        << "}";
    return oss.str();
}

catcheye::http::HttpResponse get_capture_dates(const std::filesystem::path& capture_root)
{
    std::vector<std::pair<std::string, int>> dates;
    CaptureStorageMetadata storage;
    std::error_code space_error;
    const auto space = std::filesystem::space(capture_root, space_error);
    if (space_error) {
        return {500, "Internal Server Error", catcheye::http::json_error_body("failed to read capture storage")};
    }

    storage.path = std::filesystem::absolute(capture_root).string();
    storage.total_bytes = space.capacity;
    storage.available_bytes = space.available;
    storage.used_bytes = storage.total_bytes >= storage.available_bytes
        ? storage.total_bytes - storage.available_bytes
        : 0;
    storage.used_percent = storage.total_bytes == 0
        ? 0.0
        : (static_cast<double>(storage.used_bytes) / static_cast<double>(storage.total_bytes)) * 100.0;

    if (std::filesystem::exists(capture_root)) {
        for (const auto& entry : std::filesystem::directory_iterator(capture_root)) {
            if (!entry.is_directory()) {
                continue;
            }
            const std::string date = entry.path().filename().string();
            if (!is_valid_capture_date(date) || !path_is_inside(capture_root, entry.path())) {
                continue;
            }
            int count = 0;
            for (const auto& image : std::filesystem::directory_iterator(entry.path())) {
                if (image.is_regular_file() && is_valid_capture_filename(date, image.path().filename().string())) {
                    ++count;
                    storage.capture_bytes += image.file_size();
                    ++storage.capture_count;
                }
            }
            dates.emplace_back(date, count);
        }
    }
    std::sort(dates.begin(), dates.end(), [](const auto& left, const auto& right) {
        return left.first > right.first;
    });

    std::ostringstream oss;
    oss << "{\"storage\":" << capture_storage_json(storage) << ",\"dates\":[";
    for (std::size_t i = 0; i < dates.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        oss << "{\"date\":\"" << dates[i].first << "\",\"count\":" << dates[i].second << "}";
    }
    oss << "]}";
    return {200, "OK", oss.str()};
}

catcheye::http::HttpResponse get_capture_list(const std::filesystem::path& capture_root, const catcheye::http::HttpRequest& request)
{
    const auto [path, query_text] = split_path_query(request.path);
    if (path != "/api/captures") {
        return {404, "Not Found", catcheye::http::json_error_body("unknown endpoint")};
    }

    const auto query = parse_query(query_text);
    const auto date_it = query.find("date");
    if (date_it == query.end() || !is_valid_capture_date(date_it->second)) {
        return {400, "Bad Request", catcheye::http::json_error_body("valid date query is required")};
    }
    const std::string date = date_it->second;

    int limit = 100;
    if (const auto limit_it = query.find("limit"); limit_it != query.end()) {
        try {
            limit = std::stoi(limit_it->second);
        } catch (...) {
            return {400, "Bad Request", catcheye::http::json_error_body("limit must be an integer")};
        }
    }
    if (limit <= 0) {
        return {400, "Bad Request", catcheye::http::json_error_body("limit must be positive")};
    }

    const std::string cursor = query.contains("cursor") ? query.at("cursor") : "";
    if (!cursor.empty() && !is_valid_capture_filename(date, cursor)) {
        return {400, "Bad Request", catcheye::http::json_error_body("cursor must be a capture filename")};
    }

    const auto images = list_capture_images(capture_root, date);
    std::size_t start = 0;
    if (!cursor.empty()) {
        const auto cursor_it = std::find_if(images.begin(), images.end(), [&cursor](const auto& image) {
            return image.filename == cursor;
        });
        if (cursor_it == images.end()) {
            return {400, "Bad Request", catcheye::http::json_error_body("cursor was not found")};
        }
        start = static_cast<std::size_t>(std::distance(images.begin(), cursor_it)) + 1U;
    }

    const std::size_t end = std::min(images.size(), start + static_cast<std::size_t>(limit));
    const std::string next_cursor = end < images.size() && end > start ? images[end - 1U].filename : "";

    std::ostringstream oss;
    oss << "{\"date\":\"" << date << "\",\"items\":[";
    for (std::size_t i = start; i < end; ++i) {
        if (i > start) {
            oss << ',';
        }
        oss << capture_image_metadata_json(images[i]);
    }
    oss << "],\"next_cursor\":\"" << catcheye::http::escape_json_string(next_cursor) << "\"}";
    return {200, "OK", oss.str()};
}

catcheye::http::HttpResponse get_latest_capture(const std::filesystem::path& capture_root)
{
    std::vector<CaptureImageMetadata> all_images;
    if (std::filesystem::exists(capture_root)) {
        for (const auto& entry : std::filesystem::directory_iterator(capture_root)) {
            if (!entry.is_directory()) {
                continue;
            }
            const std::string date = entry.path().filename().string();
            if (!is_valid_capture_date(date)) {
                continue;
            }
            auto images = list_capture_images(capture_root, date);
            all_images.insert(all_images.end(), std::make_move_iterator(images.begin()), std::make_move_iterator(images.end()));
        }
    }
    if (all_images.empty()) {
        return {404, "Not Found", catcheye::http::json_error_body("capture image not found")};
    }
    std::sort(all_images.begin(), all_images.end(), [](const auto& left, const auto& right) {
        return std::tie(left.date, left.filename) > std::tie(right.date, right.filename);
    });
    return {200, "OK", capture_image_metadata_json(all_images.front())};
}

catcheye::http::HttpResponse get_capture_file(
    const std::filesystem::path& capture_root,
    const catcheye::http::HttpRequest& request,
    std::string_view prefix)
{
    const std::string remainder = request.path.substr(prefix.size());
    const std::size_t slash_pos = remainder.find('/');
    if (slash_pos == std::string::npos) {
        return {400, "Bad Request", catcheye::http::json_error_body("capture file path must include date and filename")};
    }
    const std::string date = remainder.substr(0, slash_pos);
    const std::string filename = remainder.substr(slash_pos + 1U);
    if (!is_valid_capture_filename(date, filename)) {
        return {400, "Bad Request", catcheye::http::json_error_body("invalid capture file path")};
    }

    const std::filesystem::path path = capture_root / date / filename;
    if (!path_is_inside(capture_root, path)) {
        return {400, "Bad Request", catcheye::http::json_error_body("invalid capture file path")};
    }
    if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) {
        return {404, "Not Found", catcheye::http::json_error_body("capture image not found")};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {500, "Internal Server Error", catcheye::http::json_error_body("failed to open capture image")};
    }
    std::ostringstream body;
    body << input.rdbuf();
    return {200, "OK", body.str(), "image/jpeg"};
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

    server_->add_route("/api/captures/dates", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "GET") {
            return get_capture_dates(processor_->capture_dir());
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_route("/api/captures/latest", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "GET") {
            return get_latest_capture(processor_->capture_dir());
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    constexpr std::string_view capture_file_prefix = "/api/captures/file/";
    server_->add_prefix_route(std::string(capture_file_prefix), [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "GET") {
            return get_capture_file(processor_->capture_dir(), request, capture_file_prefix);
        }
        return catcheye::http::HttpResponse{405, "Method Not Allowed", catcheye::http::json_error_body("method not allowed")};
    });

    server_->add_prefix_route("/api/captures", [this](const catcheye::http::HttpRequest& request) {
        if (request.method == "GET") {
            return get_capture_list(processor_->capture_dir(), request);
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
