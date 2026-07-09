#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "catcheye/hardware/gpio_signal_config.hpp"
#include "catcheye/input/frame_source.hpp"
#include "catcheye/runtime/frame_processing_runner.hpp"
#include "catcheye/transport/websocket_publisher.hpp"
#include "capture/http_api_server.hpp"
#include "capture/processor.hpp"

namespace catcheye::capture {

struct AppOptions {
    bool show_help = false;
    catcheye::input::InputSourceConfig input;
    bool websocket_enabled = false;
    int websocket_port = 8080;
    int http_port = 8090;
    std::string gpio_chip_path = "/dev/gpiochip4";
    int trigger_gpio = -1;
    bool trigger_active_low = false;
    int trigger_debounce_ms = 200;
    int complete_gpio = -1;
    bool complete_active_low = false;
    int complete_pulse_ms = 200;
    int heartbeat_led_gpio = 13;
    bool heartbeat_led_active_low = false;
    int heartbeat_led_interval_ms = 1000;
    std::string capture_dir = "captures";
    std::string recording_dir = "recordings";
    std::string camera_properties_path = "config/camera_properties.json";
    int jpeg_quality = 95;
};

struct AppBootstrap {
    CaptureProcessorConfig processor_config;
    catcheye::runtime::RuntimeConfig runtime_config;
    bool websocket_enabled = false;
    catcheye::transport::WebSocketPublisherConfig websocket_publisher_config;
    HttpApiServerConfig http_api_server_config;
    catcheye::GpioSignalConfig heartbeat_led_config;
    std::chrono::milliseconds heartbeat_led_interval{1000};
    std::unique_ptr<catcheye::input::FrameSource> source;
};

AppOptions parse_app_options(int argc, char** argv);
AppBootstrap build_app_bootstrap(const AppOptions& options);
int run_app(int argc, char** argv);

} // namespace catcheye::capture
