#pragma once

#include <memory>
#include <string>

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
    std::string capture_dir = "captures";
    std::string recording_dir = "recordings";
    int jpeg_quality = 95;
};

struct AppBootstrap {
    CaptureProcessorConfig processor_config;
    catcheye::runtime::RuntimeConfig runtime_config;
    bool websocket_enabled = false;
    catcheye::transport::WebSocketPublisherConfig websocket_publisher_config;
    HttpApiServerConfig http_api_server_config;
    std::unique_ptr<catcheye::input::FrameSource> source;
};

AppOptions parse_app_options(int argc, char** argv);
AppBootstrap build_app_bootstrap(const AppOptions& options);
int run_app(int argc, char** argv);

} // namespace catcheye::capture
