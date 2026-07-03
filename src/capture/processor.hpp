#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "catcheye/hardware/gpio_signal_config.hpp"
#include "catcheye/runtime/frame_processor.hpp"
#include "capture/recording_controller.hpp"
#include "capture/signal.hpp"

namespace catcheye::capture {

struct CaptureProcessorConfig {
    std::string capture_dir = "captures";
    int jpeg_quality = 95;
    std::chrono::milliseconds complete_pulse_duration{200};
    std::string recording_dir = "recordings";
    catcheye::GpioInputConfig trigger_gpio;
    catcheye::GpioSignalConfig complete_gpio;
};

struct CaptureStatus {
    bool trigger_gpio_enabled = false;
    bool complete_gpio_enabled = false;
    bool busy = false;
    bool capture_requested = false;
    std::uint64_t capture_count = 0;
    std::uint64_t ignored_trigger_count = 0;
    std::string last_saved_path;
    std::string last_error;
};

class CaptureProcessor final : public catcheye::runtime::FrameProcessor {
  public:
    explicit CaptureProcessor(CaptureProcessorConfig config);
    CaptureProcessor(
        CaptureProcessorConfig config,
        std::unique_ptr<CaptureTriggerSignal> trigger_signal,
        std::unique_ptr<CaptureCompleteSignal> complete_signal);
    ~CaptureProcessor() override;

    bool initialize() override;
    catcheye::runtime::ProcessOutput process(
        const catcheye::input::Frame& frame,
        const catcheye::runtime::ProcessContext& context) override;

    void request_capture();
    CaptureStatus status() const;
    std::string status_json() const;
    RecordingStatus recording_status() const;
    RecordingStatus start_recording();
    RecordingStatus pause_recording();
    RecordingStatus resume_recording();
    RecordingStatus save_recording();
    RecordingStatus cancel_recording();

  private:
    std::string build_capture_path_locked();
    bool save_frame_locked(const catcheye::input::Frame& frame, std::string& saved_path, std::string& error);
    void set_error_locked(std::string error);

    CaptureProcessorConfig config_;
    std::unique_ptr<CaptureTriggerSignal> trigger_signal_;
    std::unique_ptr<CaptureCompleteSignal> complete_signal_;
    RecordingController recording_controller_;
    mutable std::mutex mutex_;
    bool busy_ = false;
    bool capture_requested_ = false;
    std::uint64_t capture_count_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint64_t ignored_trigger_count_ = 0;
    std::string last_saved_path_;
    std::string last_error_;
};

std::string capture_status_json(const CaptureStatus& status);

} // namespace catcheye::capture
