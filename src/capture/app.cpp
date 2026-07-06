#include "capture/app.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "catcheye/hardware/gpio_signal.hpp"
#include "catcheye/runtime/frame_processing_runner.hpp"
#include "catcheye/transport/websocket_publisher.hpp"
#include "catcheye/utils/logger.hpp"

namespace catcheye::capture {
namespace {

constexpr std::string_view DEFAULT_CAMERA_PIPELINE =
    "libcamerasrc ! video/x-raw,width=2304,height=1296,framerate=15/1,format=NV12 ! queue leaky=downstream max-size-buffers=1 ! videoflip method=rotate-180";
constexpr int DEFAULT_CAMERA_WIDTH = 2304;
constexpr int DEFAULT_CAMERA_HEIGHT = 1296;

void print_usage()
{
    std::cout << "Usage:\n"
              << "  catcheye-capture [options]\n"
              << "\n"
              << "Options:\n"
              << "  -h, --help                  Show this help\n"
              << "  --camera                    Use camera input (default)\n"
              << "  --image <path>              Use image file input\n"
              << "  --video <path>              Use video file input\n"
              << "  --camera-pipeline <pipe>    Use explicit GStreamer camera pipeline\n"
              << "  --camera-device <path>      Use camera device path\n"
              << "  --camera-width <pixels>     Camera width (default: 2304 for default CSI pipeline)\n"
              << "  --camera-height <pixels>    Camera height (default: 1296 for default CSI pipeline)\n"
              << "  --ws [port]                 Publish viewer frames over WebSocket (default port: 8080)\n"
              << "  --http-port <port>          HTTP API port (default: 8090)\n"
              << "  --gpio-chip <path>          GPIO chip path (default: /dev/gpiochip4)\n"
              << "  --trigger-gpio <line>       PLC capture trigger GPIO input line\n"
              << "  --trigger-active-low        Treat trigger GPIO as active-low\n"
              << "  --trigger-debounce-ms <ms>  Trigger debounce duration (default: 200)\n"
              << "  --complete-gpio <line>      PLC capture-complete GPIO output line\n"
              << "  --complete-active-low       Drive complete GPIO active-low\n"
              << "  --complete-pulse-ms <ms>    Complete signal pulse duration (default: 200)\n"
              << "  --heartbeat-led-gpio <line>       Runtime heartbeat LED GPIO output line (default: 13)\n"
              << "  --heartbeat-led-active-low        Drive runtime heartbeat LED active-low\n"
              << "  --heartbeat-led-interval-ms <ms>  Runtime heartbeat LED blink interval (default: 1000)\n"
              << "  --capture-dir <path>        Capture output directory (default: captures)\n"
              << "  --recording-dir <path>      Viewer recording output directory (default: recordings)\n"
              << "  --jpeg-quality <1-100>      JPEG quality (default: 95)\n"
              << "\n"
              << "Examples:\n"
              << "  catcheye-capture --camera --ws --trigger-gpio 23 --complete-gpio 24 --heartbeat-led-gpio 13\n"
              << "  catcheye-capture --image samples/frame.jpg --ws --trigger-gpio 23 --complete-gpio 24 --heartbeat-led-gpio 13\n";
}

std::string_view read_required_value(std::span<char* const> args, std::size_t& index, std::string_view flag)
{
    if (index + 1 >= args.size()) {
        throw std::invalid_argument(std::string(flag) + " requires a value");
    }
    return args[++index];
}

std::string describe_runtime_mode(const AppOptions& options)
{
    if (options.input.type == catcheye::input::InputSourceType::Camera) {
        if (!options.input.camera_pipeline.empty()) {
            return options.websocket_enabled ? "csi camera + gstreamer source + PLC capture + websocket output"
                                             : "csi camera + gstreamer source + PLC capture";
        }
        if (!options.input.camera_device.empty()) {
            return options.websocket_enabled ? "usb camera + gstreamer source + PLC capture + websocket output"
                                             : "usb camera + gstreamer source + PLC capture";
        }
        return options.websocket_enabled ? "csi camera + libcamera source + PLC capture + websocket output"
                                         : "csi camera + libcamera source + PLC capture";
    }

    const std::string mode = options.input.type == catcheye::input::InputSourceType::ImageFile
        ? "image file + gstreamer source + PLC capture"
        : "video file + gstreamer source + PLC capture";
    return options.websocket_enabled ? mode + " + websocket output" : mode;
}

bool is_input_mode(std::string_view arg)
{
    return arg == "--image" || arg == "--video" || arg == "--camera";
}

class HeartbeatLedBlinker {
  public:
    HeartbeatLedBlinker(catcheye::GpioSignalConfig config, std::chrono::milliseconds interval)
        : enabled_(config.enabled),
          signal_(std::move(config)),
          interval_(interval) {}

    ~HeartbeatLedBlinker()
    {
        stop();
    }

    HeartbeatLedBlinker(const HeartbeatLedBlinker&) = delete;
    HeartbeatLedBlinker& operator=(const HeartbeatLedBlinker&) = delete;

    bool start()
    {
        if (!enabled_) {
            return true;
        }
        if (!signal_.initialize()) {
            return false;
        }

        worker_ = std::thread([this]() {
            bool active = false;
            while (!stop_requested_.load()) {
                active = !active;
                signal_.set_state(active);
                std::this_thread::sleep_for(interval_);
            }
            signal_.set_state(false);
        });
        return true;
    }

    void stop()
    {
        stop_requested_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        signal_.shutdown();
    }

  private:
    bool enabled_ = false;
    catcheye::hardware::GpioStateSignal signal_;
    std::chrono::milliseconds interval_;
    std::atomic_bool stop_requested_{false};
    std::thread worker_;
};

} // namespace

AppOptions parse_app_options(int argc, char** argv)
{
    AppOptions options;
    const std::span<char* const> args(argv, static_cast<std::size_t>(argc));
    bool input_mode_selected = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg(args[i]);
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
        } else if (arg == "--image") {
            if (input_mode_selected) {
                throw std::runtime_error("input mode flags are mutually exclusive");
            }
            input_mode_selected = true;
            options.input.type = catcheye::input::InputSourceType::ImageFile;
            options.input.uri = read_required_value(args, i, arg);
        } else if (arg == "--video") {
            if (input_mode_selected) {
                throw std::runtime_error("input mode flags are mutually exclusive");
            }
            input_mode_selected = true;
            options.input.type = catcheye::input::InputSourceType::VideoFile;
            options.input.uri = read_required_value(args, i, arg);
        } else if (arg == "--camera") {
            if (input_mode_selected) {
                throw std::runtime_error("input mode flags are mutually exclusive");
            }
            input_mode_selected = true;
            options.input.type = catcheye::input::InputSourceType::Camera;
        } else if (arg == "--camera-pipeline") {
            options.input.camera_pipeline = read_required_value(args, i, arg);
        } else if (arg == "--camera-device") {
            options.input.camera_device = read_required_value(args, i, arg);
        } else if (arg == "--camera-width") {
            options.input.camera_width = std::stoi(std::string(read_required_value(args, i, arg)));
        } else if (arg == "--camera-height") {
            options.input.camera_height = std::stoi(std::string(read_required_value(args, i, arg)));
        } else if (arg == "--ws") {
            if (options.websocket_enabled) {
                throw std::runtime_error("--ws can only be specified once");
            }
            options.websocket_enabled = true;
            if (i + 1 < args.size() && args[i + 1][0] != '-') {
                options.websocket_port = std::stoi(std::string(read_required_value(args, i, arg)));
            }
        } else if (arg == "--http-port") {
            options.http_port = std::stoi(std::string(read_required_value(args, i, arg)));
        } else if (arg == "--gpio-chip") {
            options.gpio_chip_path = read_required_value(args, i, arg);
        } else if (arg == "--trigger-gpio") {
            options.trigger_gpio = std::stoi(std::string(read_required_value(args, i, arg)));
        } else if (arg == "--trigger-active-low") {
            options.trigger_active_low = true;
        } else if (arg == "--trigger-debounce-ms") {
            options.trigger_debounce_ms = std::stoi(std::string(read_required_value(args, i, arg)));
        } else if (arg == "--complete-gpio") {
            options.complete_gpio = std::stoi(std::string(read_required_value(args, i, arg)));
        } else if (arg == "--complete-active-low") {
            options.complete_active_low = true;
        } else if (arg == "--complete-pulse-ms") {
            options.complete_pulse_ms = std::stoi(std::string(read_required_value(args, i, arg)));
        } else if (arg == "--heartbeat-led-gpio") {
            options.heartbeat_led_gpio = std::stoi(std::string(read_required_value(args, i, arg)));
        } else if (arg == "--heartbeat-led-active-low") {
            options.heartbeat_led_active_low = true;
        } else if (arg == "--heartbeat-led-interval-ms") {
            options.heartbeat_led_interval_ms = std::stoi(std::string(read_required_value(args, i, arg)));
        } else if (arg == "--capture-dir") {
            options.capture_dir = read_required_value(args, i, arg);
        } else if (arg == "--recording-dir") {
            options.recording_dir = read_required_value(args, i, arg);
        } else if (arg == "--jpeg-quality") {
            options.jpeg_quality = std::stoi(std::string(read_required_value(args, i, arg)));
        } else if (is_input_mode(arg)) {
            throw std::runtime_error("input mode flags are mutually exclusive");
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        } else {
            throw std::invalid_argument("unexpected positional argument: " + std::string(arg));
        }
    }

    if (options.show_help) {
        return options;
    }

    if ((options.input.type == catcheye::input::InputSourceType::ImageFile ||
         options.input.type == catcheye::input::InputSourceType::VideoFile) &&
        (!options.input.camera_pipeline.empty() || !options.input.camera_device.empty())) {
        throw std::runtime_error("--camera-pipeline and --camera-device are only valid with --camera");
    }
    if (!options.input.camera_pipeline.empty() &&
        (options.input.camera_width != 1280 || options.input.camera_height != 720)) {
        throw std::runtime_error("--camera-width and --camera-height are not supported with --camera-pipeline");
    }
    if (!options.input.camera_pipeline.empty() && !options.input.camera_device.empty()) {
        throw std::runtime_error("--camera-pipeline and --camera-device cannot be used together");
    }
    if ((options.input.type == catcheye::input::InputSourceType::ImageFile ||
         options.input.type == catcheye::input::InputSourceType::VideoFile) &&
        (options.input.camera_width != 1280 || options.input.camera_height != 720)) {
        throw std::runtime_error("--camera-width and --camera-height are only valid with --camera");
    }
    if (options.input.camera_width <= 0 || options.input.camera_height <= 0) {
        throw std::runtime_error("camera dimensions must be positive integers");
    }
    if ((options.input.camera_width % 2) != 0 || (options.input.camera_height % 2) != 0) {
        throw std::runtime_error("camera dimensions must be even for NV12 output");
    }
    if (options.http_port <= 0) {
        throw std::runtime_error("HTTP port must be a positive integer");
    }
    if (options.websocket_enabled && options.websocket_port <= 0) {
        throw std::runtime_error("WebSocket port must be a positive integer");
    }
    if (options.trigger_gpio < -1) {
        throw std::runtime_error("trigger GPIO line must be -1 or a non-negative integer");
    }
    if (options.complete_gpio < -1) {
        throw std::runtime_error("complete GPIO line must be -1 or a non-negative integer");
    }
    if (options.heartbeat_led_gpio < -1) {
        throw std::runtime_error("heartbeat LED GPIO line must be -1 or a non-negative integer");
    }
    if (options.trigger_debounce_ms < 0) {
        throw std::runtime_error("trigger debounce must be zero or a positive integer");
    }
    if (options.complete_pulse_ms < 0) {
        throw std::runtime_error("complete pulse duration must be zero or a positive integer");
    }
    if (options.heartbeat_led_interval_ms <= 0) {
        throw std::runtime_error("heartbeat LED interval must be a positive integer");
    }
    if (options.trigger_gpio >= 0 && options.complete_gpio >= 0 && options.trigger_gpio == options.complete_gpio) {
        throw std::runtime_error("trigger GPIO and complete GPIO must use different lines");
    }
    if (options.heartbeat_led_gpio >= 0 && options.trigger_gpio >= 0 && options.heartbeat_led_gpio == options.trigger_gpio) {
        throw std::runtime_error("heartbeat LED GPIO and trigger GPIO must use different lines");
    }
    if (options.heartbeat_led_gpio >= 0 && options.complete_gpio >= 0 && options.heartbeat_led_gpio == options.complete_gpio) {
        throw std::runtime_error("heartbeat LED GPIO and complete GPIO must use different lines");
    }
    if (options.capture_dir.empty()) {
        throw std::runtime_error("capture directory must not be empty");
    }
    if (options.recording_dir.empty()) {
        throw std::runtime_error("recording directory must not be empty");
    }
    if (options.jpeg_quality < 1 || options.jpeg_quality > 100) {
        throw std::runtime_error("jpeg quality must be between 1 and 100");
    }

    if (options.input.type == catcheye::input::InputSourceType::Camera &&
        options.input.camera_pipeline.empty() &&
        options.input.camera_device.empty()) {
        options.input.camera_pipeline = std::string(DEFAULT_CAMERA_PIPELINE);
        options.input.camera_width = DEFAULT_CAMERA_WIDTH;
        options.input.camera_height = DEFAULT_CAMERA_HEIGHT;
    }

    if ((options.input.type == catcheye::input::InputSourceType::ImageFile ||
         options.input.type == catcheye::input::InputSourceType::VideoFile) &&
        options.input.uri.empty()) {
        throw std::runtime_error("input path is required for --image or --video");
    }

    return options;
}

AppBootstrap build_app_bootstrap(const AppOptions& options)
{
    AppBootstrap bootstrap;
    bootstrap.processor_config.capture_dir = options.capture_dir;
    bootstrap.processor_config.recording_dir = options.recording_dir;
    bootstrap.processor_config.jpeg_quality = options.jpeg_quality;
    bootstrap.processor_config.complete_pulse_duration = std::chrono::milliseconds(options.complete_pulse_ms);
    bootstrap.processor_config.trigger_gpio.enabled = options.trigger_gpio >= 0;
    bootstrap.processor_config.trigger_gpio.chip_path = options.gpio_chip_path;
    bootstrap.processor_config.trigger_gpio.line = options.trigger_gpio;
    bootstrap.processor_config.trigger_gpio.active_low = options.trigger_active_low;
    bootstrap.processor_config.trigger_gpio.debounce_duration = std::chrono::milliseconds(options.trigger_debounce_ms);
    bootstrap.processor_config.trigger_gpio.consumer = "catcheye-capture-trigger";
    bootstrap.processor_config.complete_gpio.enabled = options.complete_gpio >= 0;
    bootstrap.processor_config.complete_gpio.chip_path = options.gpio_chip_path;
    bootstrap.processor_config.complete_gpio.line = options.complete_gpio;
    bootstrap.processor_config.complete_gpio.active_low = options.complete_active_low;
    bootstrap.processor_config.complete_gpio.consumer = "catcheye-capture-complete";
    bootstrap.heartbeat_led_config.enabled = options.heartbeat_led_gpio >= 0;
    bootstrap.heartbeat_led_config.chip_path = options.gpio_chip_path;
    bootstrap.heartbeat_led_config.line = options.heartbeat_led_gpio;
    bootstrap.heartbeat_led_config.active_low = options.heartbeat_led_active_low;
    bootstrap.heartbeat_led_config.consumer = "catcheye-capture-heartbeat-led";
    bootstrap.heartbeat_led_interval = std::chrono::milliseconds(options.heartbeat_led_interval_ms);
    bootstrap.runtime_config.process_every_n_frames = 1;
    bootstrap.websocket_enabled = options.websocket_enabled;
    bootstrap.websocket_publisher_config.port = options.websocket_port;
    bootstrap.http_api_server_config.port = options.http_port;
    bootstrap.source = catcheye::input::create_frame_source(options.input);
    return bootstrap;
}

int run_app(int argc, char** argv)
{
    const AppOptions options = parse_app_options(argc, argv);
    if (options.show_help) {
        print_usage();
        return 0;
    }

    AppBootstrap bootstrap = build_app_bootstrap(options);

    if (const auto log = logger()) {
        log->info(
            "catcheye-capture starting (mode='{}', capture_dir='{}', http_port={}, ws_port={})",
            describe_runtime_mode(options),
            bootstrap.processor_config.capture_dir,
            bootstrap.http_api_server_config.port,
            bootstrap.websocket_publisher_config.port);
        if (bootstrap.processor_config.trigger_gpio.enabled) {
            log->info(
                "trigger GPIO enabled: chip='{}', line={}, active_low={}, debounce_ms={}",
                bootstrap.processor_config.trigger_gpio.chip_path,
                bootstrap.processor_config.trigger_gpio.line,
                bootstrap.processor_config.trigger_gpio.active_low,
                bootstrap.processor_config.trigger_gpio.debounce_duration.count());
        }
        if (bootstrap.processor_config.complete_gpio.enabled) {
            log->info(
                "complete GPIO enabled: chip='{}', line={}, active_low={}, pulse_ms={}",
                bootstrap.processor_config.complete_gpio.chip_path,
                bootstrap.processor_config.complete_gpio.line,
                bootstrap.processor_config.complete_gpio.active_low,
                bootstrap.processor_config.complete_pulse_duration.count());
        }
        if (bootstrap.heartbeat_led_config.enabled) {
            log->info(
                "heartbeat LED enabled: chip='{}', line={}, active_low={}, interval_ms={}",
                bootstrap.heartbeat_led_config.chip_path,
                bootstrap.heartbeat_led_config.line,
                bootstrap.heartbeat_led_config.active_low,
                bootstrap.heartbeat_led_interval.count());
        }
    }

    HeartbeatLedBlinker heartbeat_led(bootstrap.heartbeat_led_config, bootstrap.heartbeat_led_interval);
    if (!heartbeat_led.start()) {
        throw std::runtime_error("failed to initialize heartbeat LED GPIO output");
    }

    auto processor = std::make_unique<CaptureProcessor>(std::move(bootstrap.processor_config));
    auto* processor_ptr = processor.get();
    auto* camera_source_ptr = bootstrap.source.get();
    auto http_api_server = std::make_unique<HttpApiServer>(
        bootstrap.http_api_server_config,
        processor_ptr,
        camera_source_ptr);
    if (!http_api_server->start()) {
        throw std::runtime_error("failed to start HTTP API server");
    }

    std::unique_ptr<catcheye::transport::ResultPublisher> publisher;
    if (bootstrap.websocket_enabled) {
        publisher = std::make_unique<catcheye::transport::WebSocketPublisher>(
            bootstrap.websocket_publisher_config);
    }

    catcheye::runtime::FrameProcessingRunner runner(
        std::move(bootstrap.runtime_config),
        std::move(bootstrap.source),
        std::move(processor),
        std::move(publisher));
    const int exit_code = runner.run();
    http_api_server->stop();
    return exit_code;
}

} // namespace catcheye::capture
