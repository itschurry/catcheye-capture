#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

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

std::string http_get(int port, std::string_view path)
{
    const int sock_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(sock_fd >= 0, "failed to create test socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    require(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1, "failed to parse test address");
    require(::connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "failed to connect test socket");

    const std::string request = "GET " + std::string(path) + " HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
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

    require(response.rfind("HTTP/1.1 200 OK\r\n", 0) == 0, "HTTP response status mismatch");
    const std::size_t body_pos = response.find("\r\n\r\n");
    require(body_pos != std::string::npos, "HTTP response body missing");
    return response.substr(body_pos + 4U);
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

    server.stop();
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
    test_capture_metadata_for_viewer();
    test_http_device_info_and_capture_status();
    test_busy_trigger_is_ignored();
    std::cout << "capture tests passed\n";
    return 0;
}
