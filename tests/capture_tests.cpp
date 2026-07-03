#include <cassert>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "catcheye/input/pixel_format.hpp"
#include "capture/app.hpp"
#include "capture/processor.hpp"

namespace {

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
    assert(options.http_port == 8090);
    assert(options.capture_dir == "captures");
    assert(options.jpeg_quality == 95);
    assert(!options.websocket_enabled);
    assert(options.websocket_port == 8080);
    assert(options.trigger_gpio == -1);
    assert(options.complete_gpio == -1);
    assert(options.trigger_debounce_ms == 200);
    assert(options.complete_pulse_ms == 200);
}

void test_parse_validation()
{
    assert(throws_for({"catcheye-capture", "--trigger-gpio", "23", "--complete-gpio", "23"}));
    assert(throws_for({"catcheye-capture", "--trigger-debounce-ms", "-1"}));
    assert(throws_for({"catcheye-capture", "--complete-pulse-ms", "-1"}));
    assert(throws_for({"catcheye-capture", "--jpeg-quality", "0"}));
    assert(throws_for({"catcheye-capture", "--jpeg-quality", "101"}));
    assert(throws_for({"catcheye-capture", "--ws", "-1"}));
    auto options = parse({"catcheye-capture", "--ws", "8099"});
    assert(options.websocket_enabled);
    assert(options.websocket_port == 8099);
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
    assert(processor.initialize());

    trigger_ptr->trigger();
    processor.process(make_test_frame(), {});
    trigger_ptr->trigger();
    processor.process(make_test_frame(), {});

    const auto status = processor.status();
    assert(status.capture_count == 2);
    assert(status.last_error.empty());
    assert(complete_ptr->pulse_count == 2);
    assert(complete_ptr->last_duration == std::chrono::milliseconds(7));
    assert(std::filesystem::exists(status.last_saved_path));
    assert(status.last_saved_path.find(".jpg") != std::string::npos);
    assert(std::filesystem::path(status.last_saved_path).parent_path().filename().string().size() == 10);

    int jpeg_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(output_root)) {
        if (entry.path().extension() == ".jpg") {
            const auto image = cv::imread(entry.path().string());
            assert(!image.empty());
            ++jpeg_count;
        }
    }
    assert(jpeg_count == 2);

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
    assert(processor.initialize());

    const auto output = processor.process(
        make_test_frame(),
        {.frame_index = 1, .should_process = true, .needs_publish = true});
    assert(output.has_message);
    assert(output.has_publish_frame);
    assert(output.message.stream_name == "capture-viewer");
    assert(output.message.metadata_json.find("\"kind\":\"capture\"") != std::string::npos);
    assert(output.message.metadata_json.find("\"capture_connected\":true") != std::string::npos);
    assert(output.message.metadata_json.find("\"roi_enabled\":false") != std::string::npos);
    assert(output.message.metadata_json.find("\"recording_enabled\":false") != std::string::npos);

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
    assert(processor.initialize());

    trigger_ptr->trigger();
    processor.process(make_test_frame(), {});

    const auto status = processor.status();
    assert(status.capture_count == 1);
    assert(status.ignored_trigger_count == 1);
    assert(status.last_error == "capture request ignored while busy");

    std::filesystem::remove_all(output_root);
}

} // namespace

int main()
{
    test_parse_defaults();
    test_parse_validation();
    test_capture_save_and_sequence();
    test_capture_metadata_for_viewer();
    test_busy_trigger_is_ignored();
    std::cout << "capture tests passed\n";
    return 0;
}
