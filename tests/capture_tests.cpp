#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "catcheye/input/pixel_format.hpp"
#include "capture/app.hpp"
#include "capture/http_api_server.hpp"
#include "capture/processor.hpp"

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<char*> argv_from(std::vector<std::string>& args)
{
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return argv;
}

catcheye::capture::AppOptions parse(std::vector<std::string> args)
{
    std::vector<char*> argv = argv_from(args);
    return catcheye::capture::parse_app_options(static_cast<int>(argv.size()), argv.data());
}

bool throws_for(std::vector<std::string> args)
{
    try {
        (void)parse(std::move(args));
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

struct HttpTestResponse {
    int status_code = 0;
    std::string content_type;
    std::string body;
};

std::string http_header_value(const std::string& headers, std::string_view name)
{
    const std::string needle = std::string(name) + ": ";
    std::istringstream stream(headers);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind(needle, 0) == 0) {
            return line.substr(needle.size());
        }
    }
    return {};
}

HttpTestResponse http_request_raw(int port, std::string_view method, std::string_view path)
{
    const int sock_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(sock_fd >= 0, "failed to create test socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    require(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1, "failed to parse test address");
    require(::connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "failed to connect test socket");

    const std::string request = std::string(method) + " " + std::string(path) + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    require(::send(sock_fd, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()), "failed to send HTTP request");

    std::string response;
    char buffer[4096]{};
    while (true) {
        const ssize_t received = ::recv(sock_fd, buffer, sizeof(buffer), 0);
        if (received == 0) {
            break;
        }
        require(received > 0, "failed to receive HTTP response");
        response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(sock_fd);

    require(response.rfind("HTTP/1.1 ", 0) == 0, "HTTP response status line missing");
    const std::size_t body_pos = response.find("\r\n\r\n");
    require(body_pos != std::string::npos, "HTTP response body missing");
    const std::string status_text = response.substr(9U, 3U);
    const std::string headers = response.substr(0, body_pos + 2U);
    return {
        .status_code = std::stoi(status_text),
        .content_type = http_header_value(headers, "Content-Type"),
        .body = response.substr(body_pos + 4U),
    };
}

std::string http_request(int port, std::string_view method, std::string_view path)
{
    const auto response = http_request_raw(port, method, path);
    require(response.status_code == 200, "HTTP response status mismatch");
    return response.body;
}

std::string http_get(int port, std::string_view path)
{
    return http_request(port, "GET", path);
}

std::string http_post(int port, std::string_view path)
{
    return http_request(port, "POST", path);
}

std::string today_dir_name()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&timestamp, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d");
    return oss.str();
}

std::filesystem::path today_capture_dir(const std::filesystem::path& root)
{
    return root / today_dir_name();
}

void write_test_jpeg(const std::filesystem::path& path, int value)
{
    std::filesystem::create_directories(path.parent_path());
    const cv::Mat image(4, 4, CV_8UC3, cv::Scalar(value, value, value));
    require(cv::imwrite(path.string(), image), "failed to create test JPEG");
}

int count_files_with_prefix(const std::filesystem::path& root, std::string_view prefix)
{
    if (!std::filesystem::exists(root)) {
        return 0;
    }

    int count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.path().filename().string().rfind(std::string(prefix), 0) == 0) {
            ++count;
        }
    }
    return count;
}

bool ends_with(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size()
        && value.substr(value.size() - suffix.size()) == suffix;
}

std::uintmax_t json_uint_field(const std::string& json, std::string_view field)
{
    const std::string needle = "\"" + std::string(field) + "\":";
    const std::size_t start = json.find(needle);
    require(start != std::string::npos, "JSON uint field missing: " + std::string(field));
    std::size_t pos = start + needle.size();
    std::uintmax_t value = 0;
    require(pos < json.size() && json[pos] >= '0' && json[pos] <= '9', "JSON uint field is not numeric: " + std::string(field));
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        value = (value * 10U) + static_cast<std::uintmax_t>(json[pos] - '0');
        ++pos;
    }
    return value;
}

void wait_for_fresh_second()
{
    const std::time_t start = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    while (std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) == start) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

catcheye::input::Frame make_test_frame()
{
    catcheye::input::Frame frame;
    frame.width = 4;
    frame.height = 4;
    frame.stride = 12;
    frame.format = catcheye::input::PixelFormat::BGR;
    frame.data.resize(static_cast<std::size_t>(frame.stride * frame.height), 127);
    return frame;
}

catcheye::input::Frame make_recording_frame()
{
    catcheye::input::Frame frame;
    frame.width = 64;
    frame.height = 64;
    frame.stride = 192;
    frame.format = catcheye::input::PixelFormat::BGR;
    frame.data.resize(static_cast<std::size_t>(frame.stride * frame.height), 127);
    return frame;
}

class FakeTriggerSignal final : public catcheye::capture::CaptureTriggerSignal {
  public:
    bool initialize(Callback callback) override
    {
        callback_ = std::move(callback);
        initialized = true;
        return true;
    }

    void shutdown() override
    {
        initialized = false;
    }

    void trigger()
    {
        if (callback_) {
            callback_();
        }
    }

    bool initialized = false;

  private:
    Callback callback_;
};

class FakeCompleteSignal final : public catcheye::capture::CaptureCompleteSignal {
  public:
    explicit FakeCompleteSignal(std::function<void()> on_pulse = {})
        : on_pulse_(std::move(on_pulse)) {}

    bool initialize() override
    {
        initialized = true;
        return true;
    }

    void pulse(std::chrono::milliseconds duration) override
    {
        last_duration = duration;
        ++pulse_count;
        if (on_pulse_) {
            on_pulse_();
        }
    }

    void shutdown() override
    {
        initialized = false;
    }

    bool initialized = false;
    int pulse_count = 0;
    std::chrono::milliseconds last_duration{0};

  private:
    std::function<void()> on_pulse_;
};

void test_parse_defaults()
{
    auto options = parse({"catcheye-capture"});
    require(options.http_port == 8090, "default HTTP port mismatch");
    require(options.capture_dir == "captures", "default capture dir mismatch");
    require(options.recording_dir == "recordings", "default recording dir mismatch");
    require(options.jpeg_quality == 95, "default JPEG quality mismatch");
    require(!options.websocket_enabled, "websocket should be disabled by default");
    require(options.websocket_port == 8080, "default websocket port mismatch");
    require(options.trigger_gpio == -1, "default trigger GPIO mismatch");
    require(options.complete_gpio == -1, "default complete GPIO mismatch");
    require(options.trigger_debounce_ms == 200, "default trigger debounce mismatch");
    require(options.complete_pulse_ms == 200, "default complete pulse mismatch");
}

void test_parse_validation()
{
    require(throws_for({"catcheye-capture", "--trigger-gpio", "23", "--complete-gpio", "23"}), "same GPIO lines should be rejected");
    require(throws_for({"catcheye-capture", "--trigger-debounce-ms", "-1"}), "negative debounce should be rejected");
    require(throws_for({"catcheye-capture", "--complete-pulse-ms", "-1"}), "negative pulse should be rejected");
    require(throws_for({"catcheye-capture", "--jpeg-quality", "0"}), "low JPEG quality should be rejected");
    require(throws_for({"catcheye-capture", "--jpeg-quality", "101"}), "high JPEG quality should be rejected");
    require(throws_for({"catcheye-capture", "--recording-dir", ""}), "empty recording dir should be rejected");
    require(throws_for({"catcheye-capture", "--ws", "-1"}), "negative websocket port should be rejected");
    auto options = parse({"catcheye-capture", "--ws", "8099"});
    require(options.websocket_enabled, "websocket should be enabled");
    require(options.websocket_port == 8099, "websocket port mismatch");
}

void test_capture_save_and_sequence()
{
    const auto output_root = std::filesystem::temp_directory_path() / "catcheye_capture_tests";
    std::filesystem::remove_all(output_root);

    catcheye::capture::CaptureProcessorConfig config;
    config.capture_dir = output_root.string();
    config.jpeg_quality = 90;
    config.complete_pulse_duration = std::chrono::milliseconds(7);

    auto trigger = std::make_unique<FakeTriggerSignal>();
    auto complete = std::make_unique<FakeCompleteSignal>();
    auto* trigger_ptr = trigger.get();
    auto* complete_ptr = complete.get();

    catcheye::capture::CaptureProcessor processor(
        config,
        std::move(trigger),
        std::move(complete));
    require(processor.initialize(), "processor should initialize");

    trigger_ptr->trigger();
    processor.process(make_test_frame(), {});
    trigger_ptr->trigger();
    processor.process(make_test_frame(), {});

    const auto status = processor.status();
    require(status.capture_count == 2, "capture count mismatch");
    require(status.last_error.empty(), "last error should be empty");
    require(complete_ptr->pulse_count == 2, "complete pulse count mismatch");
    require(complete_ptr->last_duration == std::chrono::milliseconds(7), "complete pulse duration mismatch");
    require(std::filesystem::exists(status.last_saved_path), "last saved JPEG should exist");
    require(status.last_saved_path.find(".jpg") != std::string::npos, "saved path should be a JPEG");
    require(std::filesystem::path(status.last_saved_path).parent_path().filename().string().size() == 10, "date directory name mismatch");

    int jpeg_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(output_root)) {
        if (entry.path().extension() == ".jpg") {
            const auto image = cv::imread(entry.path().string());
            require(!image.empty(), "saved JPEG should be readable");
            ++jpeg_count;
        }
    }
    require(jpeg_count == 2, "JPEG file count mismatch");

    std::filesystem::remove_all(output_root);
}

void test_capture_sequence_recovers_from_existing_files()
{
    const auto output_root = std::filesystem::temp_directory_path() / "catcheye_capture_recover_sequence_tests";
    std::filesystem::remove_all(output_root);

    const auto existing_path = today_capture_dir(output_root) / "120000_000_000007.jpg";
    write_test_jpeg(existing_path, 32);
    const auto existing_size = std::filesystem::file_size(existing_path);

    catcheye::capture::CaptureProcessorConfig config;
    config.capture_dir = output_root.string();

    auto trigger = std::make_unique<FakeTriggerSignal>();
    auto* trigger_ptr = trigger.get();

    catcheye::capture::CaptureProcessor processor(
        config,
        std::move(trigger),
        std::make_unique<FakeCompleteSignal>());
    require(processor.initialize(), "processor should initialize");

    trigger_ptr->trigger();
    processor.process(make_test_frame(), {});

    const auto status = processor.status();
    require(status.capture_count == 1, "capture count should stay process-local");
    require(status.last_error.empty(), "last error should be empty after recovered save");
    require(ends_with(status.last_saved_path, "_000008.jpg"), "saved sequence should continue after existing files");
    require(std::filesystem::exists(existing_path), "existing JPEG should remain");
    require(std::filesystem::file_size(existing_path) == existing_size, "existing JPEG should not be overwritten");
    require(cv::imread(status.last_saved_path).empty() == false, "saved JPEG should be readable");
    require(count_files_with_prefix(output_root, ".tmp.") == 0, "temporary JPEG should be removed");

    int jpeg_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(output_root)) {
        if (entry.path().extension() == ".jpg") {
            ++jpeg_count;
        }
    }
    require(jpeg_count == 2, "recovered save should create one new JPEG");

    std::filesystem::remove_all(output_root);
}

void test_capture_save_fails_when_temp_path_exists()
{
    const auto output_root = std::filesystem::temp_directory_path() / "catcheye_capture_temp_collision_tests";
    std::filesystem::remove_all(output_root);

    const auto today_dir = today_capture_dir(output_root);
    std::filesystem::create_directories(today_dir);

    std::ostringstream temp_name;
    temp_name << ".tmp." << static_cast<long long>(::getpid()) << ".1.jpg";
    const auto temp_path = today_dir / temp_name.str();
    write_test_jpeg(temp_path, 64);

    catcheye::capture::CaptureProcessorConfig config;
    config.capture_dir = output_root.string();

    auto trigger = std::make_unique<FakeTriggerSignal>();
    auto complete = std::make_unique<FakeCompleteSignal>();
    auto* trigger_ptr = trigger.get();
    auto* complete_ptr = complete.get();

    catcheye::capture::CaptureProcessor processor(
        config,
        std::move(trigger),
        std::move(complete));
    require(processor.initialize(), "processor should initialize");

    trigger_ptr->trigger();
    processor.process(make_test_frame(), {});

    const auto status = processor.status();
    require(status.capture_count == 0, "failed temp collision should not count as capture");
    require(complete_ptr->pulse_count == 0, "failed temp collision should not pulse complete GPIO");
    require(status.last_error.find("temporary capture path already exists") != std::string::npos, "temp collision error missing");

    std::filesystem::remove_all(output_root);
}

void test_capture_metadata_for_viewer()
{
    const auto output_root = std::filesystem::temp_directory_path() / "catcheye_capture_metadata_tests";
    std::filesystem::remove_all(output_root);

    catcheye::capture::CaptureProcessorConfig config;
    config.capture_dir = output_root.string();

    catcheye::capture::CaptureProcessor processor(
        config,
        std::make_unique<FakeTriggerSignal>(),
        std::make_unique<FakeCompleteSignal>());
    require(processor.initialize(), "processor should initialize");

    const auto output = processor.process(
        make_test_frame(),
        {.frame_index = 1, .should_process = true, .needs_publish = true});
    require(output.has_message, "viewer metadata should exist");
    require(output.has_publish_frame, "viewer frame should exist");
    require(output.message.stream_name == "capture-viewer", "viewer stream name mismatch");
    require(output.message.metadata_json.find("\"kind\":\"capture\"") != std::string::npos, "metadata kind missing");
    require(output.message.metadata_json.find("\"capture_count\":0") != std::string::npos, "capture count missing");
    require(output.message.metadata_json.find("\"ignored_trigger_count\":0") != std::string::npos, "ignored trigger count missing");
    require(output.message.metadata_json.find("\"capture_connected\"") == std::string::npos, "capture_connected should not be exposed");
    require(output.message.metadata_json.find("\"roi_enabled\"") == std::string::npos, "roi flag should not be exposed");
    require(output.message.metadata_json.find("\"recording_enabled\"") == std::string::npos, "recording flag should not be exposed");

    std::filesystem::remove_all(output_root);
}

void test_http_device_info_and_capture_status()
{
    const auto output_root = std::filesystem::temp_directory_path() / "catcheye_capture_http_tests";
    std::filesystem::remove_all(output_root);

    catcheye::capture::CaptureProcessorConfig config;
    config.capture_dir = output_root.string();
    config.trigger_gpio.enabled = true;
    config.complete_gpio.enabled = true;

    catcheye::capture::CaptureProcessor processor(
        config,
        std::make_unique<FakeTriggerSignal>(),
        std::make_unique<FakeCompleteSignal>());
    require(processor.initialize(), "processor should initialize");

    catcheye::capture::HttpApiServer server(
        {.bind_address = "127.0.0.1", .port = 18090},
        &processor,
        nullptr);
    require(server.start(), "HTTP API server should start");

    const std::string device_info = http_get(18090, "/api/device-info");
    require(device_info == R"({"app":"catcheye-capture","kind":"capture"})", "device-info response mismatch");
    require(device_info.find("capabilities") == std::string::npos, "device-info should not expose capabilities");
    require(device_info.find("person_roi_alert_disabled") == std::string::npos, "device-info should not expose ROI alert state");

    const std::string status = http_get(18090, "/api/capture/status");
    require(status.find("\"app\":\"catcheye-capture\"") != std::string::npos, "status app missing");
    require(status.find("\"kind\":\"capture\"") != std::string::npos, "status kind missing");
    require(status.find("\"trigger_gpio_enabled\":true") != std::string::npos, "trigger GPIO status missing");
    require(status.find("\"complete_gpio_enabled\":true") != std::string::npos, "complete GPIO status missing");
    require(status.find("\"capture_count\":0") != std::string::npos, "capture count status missing");
    require(status.find("\"capabilities\"") == std::string::npos, "status should not expose capabilities");
    require(status.find("\"capture_connected\"") == std::string::npos, "status should not expose capture_connected");
    require(status.find("\"roi_enabled\"") == std::string::npos, "status should not expose ROI flag");
    require(status.find("\"recording_enabled\"") == std::string::npos, "status should not expose recording flag");

    const std::string requested = http_post(18090, "/api/capture/request");
    require(requested.find("\"capture_requested\":true") != std::string::npos, "manual capture request should be pending");

    server.stop();
    std::filesystem::remove_all(output_root);
}

void test_http_recording_api()
{
    const auto output_root = std::filesystem::temp_directory_path() / "catcheye_capture_recording_tests";
    std::filesystem::remove_all(output_root);

    catcheye::capture::CaptureProcessorConfig config;
    config.capture_dir = (output_root / "captures").string();
    config.recording_dir = (output_root / "recordings").string();

    catcheye::capture::CaptureProcessor processor(
        config,
        std::make_unique<FakeTriggerSignal>(),
        std::make_unique<FakeCompleteSignal>());
    require(processor.initialize(), "processor should initialize");

    catcheye::capture::HttpApiServer server(
        {.bind_address = "127.0.0.1", .port = 18091},
        &processor,
        nullptr);
    require(server.start(), "HTTP API server should start");

    const std::string idle = http_get(18091, "/api/recording");
    require(idle.find("\"state\":\"idle\"") != std::string::npos, "recording should start idle");

    const std::string started = http_post(18091, "/api/recording/start");
    require(started.find("\"state\":\"recording\"") != std::string::npos, "recording should start");

    processor.process(make_recording_frame(), {});

    const std::string paused = http_post(18091, "/api/recording/pause");
    require(paused.find("\"state\":\"paused\"") != std::string::npos, "recording should pause");

    const std::string resumed = http_post(18091, "/api/recording/resume");
    require(resumed.find("\"state\":\"recording\"") != std::string::npos, "recording should resume");

    processor.process(make_recording_frame(), {});

    const std::string saved = http_post(18091, "/api/recording/save");
    require(saved.find("\"state\":\"idle\"") != std::string::npos, "recording should return idle after save");
    require(saved.find("\"saved_path\":\"") != std::string::npos, "recording saved path should exist");
    require(saved.find("\"written_frames\":2") != std::string::npos, "recording frame count mismatch");

    const auto status = processor.recording_status();
    require(!status.saved_path.empty(), "recording saved path should be stored");
    require(std::filesystem::exists(status.saved_path), "recording file should exist");

    server.stop();
    std::filesystem::remove_all(output_root);
}

void test_http_capture_image_api()
{
    const auto output_root = std::filesystem::temp_directory_path() / "catcheye_capture_image_api_tests";
    std::filesystem::remove_all(output_root);

    const auto older = output_root / "2026-07-02" / "091500_010_000001.jpg";
    const auto first = output_root / "2026-07-03" / "142530_015_000012.jpg";
    const auto latest = output_root / "2026-07-03" / "142531_220_000013.jpg";
    write_test_jpeg(older, 32);
    write_test_jpeg(first, 64);
    write_test_jpeg(latest, 96);
    std::filesystem::create_directories(output_root / "2026-07-03");
    {
        std::ofstream ignored(output_root / "2026-07-03" / "not-a-capture.txt");
        ignored << "ignore";
    }

    catcheye::capture::CaptureProcessorConfig config;
    config.capture_dir = output_root.string();

    catcheye::capture::CaptureProcessor processor(
        config,
        std::make_unique<FakeTriggerSignal>(),
        std::make_unique<FakeCompleteSignal>());
    require(processor.initialize(), "processor should initialize");

    catcheye::capture::HttpApiServer server(
        {.bind_address = "127.0.0.1", .port = 18092},
        &processor,
        nullptr);
    require(server.start(), "HTTP API server should start");

    const std::string dates = http_get(18092, "/api/captures/dates");
    const std::uintmax_t expected_capture_bytes =
        std::filesystem::file_size(older) +
        std::filesystem::file_size(first) +
        std::filesystem::file_size(latest);
    const auto total_bytes = json_uint_field(dates, "total_bytes");
    const auto available_bytes = json_uint_field(dates, "available_bytes");
    const auto used_bytes = json_uint_field(dates, "used_bytes");
    require(dates.find(R"("path":")") != std::string::npos, "storage path missing");
    require(dates.find(R"("used_percent":)") != std::string::npos, "storage used percent missing");
    require(total_bytes >= available_bytes, "storage total should be >= available");
    require(used_bytes + available_bytes <= total_bytes, "storage used and available should not exceed total");
    require(json_uint_field(dates, "capture_bytes") == expected_capture_bytes, "capture byte count mismatch");
    require(json_uint_field(dates, "capture_count") == 3, "capture file count mismatch");
    require(dates.find(R"({"date":"2026-07-03","count":2})") != std::string::npos, "capture date count missing");
    require(dates.find(R"({"date":"2026-07-02","count":1})") != std::string::npos, "older capture date count missing");
    require(dates.find("not-a-capture") == std::string::npos, "non-capture file should be ignored");

    const std::string list = http_get(18092, "/api/captures?date=2026-07-03&limit=1");
    require(list.find(R"("filename":"142531_220_000013.jpg")") != std::string::npos, "latest image should be first");
    require(list.find(R"("next_cursor":"142531_220_000013.jpg")") != std::string::npos, "next cursor missing");
    require(list.find(R"("width":4)") != std::string::npos, "image width missing");
    require(list.find(R"("height":4)") != std::string::npos, "image height missing");
    require(list.find(R"("captured_at":"2026-07-03T14:25:31.220)") != std::string::npos, "captured_at missing");

    const std::string page2 = http_get(18092, "/api/captures?date=2026-07-03&limit=1&cursor=142531_220_000013.jpg");
    require(page2.find(R"("filename":"142530_015_000012.jpg")") != std::string::npos, "cursor page image missing");
    require(page2.find(R"("next_cursor":"")") != std::string::npos, "last page cursor should be empty");

    const std::string latest_json = http_get(18092, "/api/captures/latest");
    require(latest_json.find(R"("filename":"142531_220_000013.jpg")") != std::string::npos, "latest endpoint mismatch");

    const auto file = http_request_raw(18092, "GET", "/api/captures/file/2026-07-03/142531_220_000013.jpg");
    require(file.status_code == 200, "capture file should return 200");
    require(file.content_type == "image/jpeg", "capture file content type mismatch");
    require(file.body.size() == std::filesystem::file_size(latest), "capture file size mismatch");

    const auto missing = http_request_raw(18092, "GET", "/api/captures/file/2026-07-03/142531_220_999999.jpg");
    require(missing.status_code == 404, "missing capture file should return 404");

    const auto escaped = http_request_raw(18092, "GET", "/api/captures/file/2026-07-03/../secret.jpg");
    require(escaped.status_code == 400, "path traversal should return 400");

    server.stop();
    std::filesystem::remove_all(output_root);
}

void test_recording_start_fails_when_path_exists()
{
    const auto output_root = std::filesystem::temp_directory_path() / "catcheye_capture_recording_collision_tests";
    std::filesystem::remove_all(output_root);
    std::filesystem::create_directories(output_root);

    wait_for_fresh_second();
    catcheye::capture::RecordingController first(output_root.string());
    const auto first_status = first.start();
    require(first_status.state == catcheye::capture::RecordingState::Recording, "first recording should start");
    {
        std::ofstream file(first_status.active_path);
        file << "existing";
    }

    catcheye::capture::RecordingController second(output_root.string());
    const auto second_status = second.start();
    require(second_status.state == catcheye::capture::RecordingState::Idle, "second recording should stay idle on path collision");
    require(second_status.error.find("recording path already exists") != std::string::npos, "recording collision error missing");

    first.cancel();
    std::filesystem::remove_all(output_root);
}

void test_busy_trigger_is_ignored()
{
    const auto output_root = std::filesystem::temp_directory_path() / "catcheye_capture_busy_tests";
    std::filesystem::remove_all(output_root);

    catcheye::capture::CaptureProcessorConfig config;
    config.capture_dir = output_root.string();

    FakeTriggerSignal* trigger_ptr = nullptr;
    auto trigger = std::make_unique<FakeTriggerSignal>();
    trigger_ptr = trigger.get();
    auto complete = std::make_unique<FakeCompleteSignal>([&]() {
        trigger_ptr->trigger();
    });

    catcheye::capture::CaptureProcessor processor(
        config,
        std::move(trigger),
        std::move(complete));
    require(processor.initialize(), "processor should initialize");

    trigger_ptr->trigger();
    processor.process(make_test_frame(), {});

    const auto status = processor.status();
    require(status.capture_count == 1, "capture count mismatch after busy trigger");
    require(status.ignored_trigger_count == 1, "ignored trigger count mismatch");
    require(status.last_error == "capture request ignored while busy", "busy trigger error mismatch");

    std::filesystem::remove_all(output_root);
}

} // namespace

int main()
{
    test_parse_defaults();
    test_parse_validation();
    test_capture_save_and_sequence();
    test_capture_sequence_recovers_from_existing_files();
    test_capture_save_fails_when_temp_path_exists();
    test_capture_metadata_for_viewer();
    test_http_device_info_and_capture_status();
    test_http_recording_api();
    test_http_capture_image_api();
    test_recording_start_fails_when_path_exists();
    test_busy_trigger_is_ignored();
    std::cout << "capture tests passed\n";
    return 0;
}
