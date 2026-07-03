#include "capture/processor.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "catcheye/utils/logger.hpp"
#include "capture/frame_conversion.hpp"

namespace catcheye::capture {
namespace {

std::string json_escape(const std::string& value)
{
    std::ostringstream oss;
    for (const char c : value) {
        switch (c) {
            case '\\':
                oss << "\\\\";
                break;
            case '"':
                oss << "\\\"";
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
                oss << c;
                break;
        }
    }
    return oss.str();
}

std::tm local_time(std::time_t timestamp)
{
    std::tm tm{};
    localtime_r(&timestamp, &tm);
    return tm;
}

} // namespace

CaptureProcessor::CaptureProcessor(CaptureProcessorConfig config)
    : CaptureProcessor(
          config,
          make_gpio_trigger_signal(config.trigger_gpio),
          make_gpio_complete_signal(config.complete_gpio))
{
}

CaptureProcessor::CaptureProcessor(
    CaptureProcessorConfig config,
    std::unique_ptr<CaptureTriggerSignal> trigger_signal,
    std::unique_ptr<CaptureCompleteSignal> complete_signal)
    : config_(std::move(config)),
      trigger_signal_(std::move(trigger_signal)),
      complete_signal_(std::move(complete_signal))
{
}

CaptureProcessor::~CaptureProcessor()
{
    if (trigger_signal_ != nullptr) {
        trigger_signal_->shutdown();
    }
    if (complete_signal_ != nullptr) {
        complete_signal_->shutdown();
    }
}

bool CaptureProcessor::initialize()
{
    if (config_.capture_dir.empty()) {
        set_error_locked("capture directory must not be empty");
        return false;
    }
    if (config_.jpeg_quality < 1 || config_.jpeg_quality > 100) {
        set_error_locked("jpeg quality must be between 1 and 100");
        return false;
    }
    if (config_.complete_pulse_duration.count() < 0) {
        set_error_locked("complete pulse duration must not be negative");
        return false;
    }

    std::filesystem::create_directories(config_.capture_dir);

    if (trigger_signal_ != nullptr && !trigger_signal_->initialize([this]() { request_capture(); })) {
        set_error_locked("failed to initialize trigger GPIO input");
        return false;
    }
    if (complete_signal_ != nullptr && !complete_signal_->initialize()) {
        set_error_locked("failed to initialize complete GPIO output");
        return false;
    }

    if (const auto log = logger()) {
        log->info("CaptureProcessor ready: capture_dir='{}', jpeg_quality={}", config_.capture_dir, config_.jpeg_quality);
    }
    return true;
}

void CaptureProcessor::request_capture()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ || capture_requested_) {
        ++ignored_trigger_count_;
        last_error_ = "capture request ignored while busy";
        return;
    }
    capture_requested_ = true;
    last_error_.clear();
}

catcheye::runtime::ProcessOutput CaptureProcessor::process(
    const catcheye::input::Frame& frame,
    const catcheye::runtime::ProcessContext& context)
{
    bool had_capture_request = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capture_requested_) {
            capture_requested_ = false;
            busy_ = true;
            had_capture_request = true;
        }
    }

    if (had_capture_request) {
        std::string saved_path;
        std::string error;
        bool saved = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            saved = save_frame_locked(frame, saved_path, error);
            if (saved) {
                last_saved_path_ = saved_path;
                last_error_.clear();
                ++capture_count_;
            } else {
                last_error_ = error;
            }
        }

        if (saved) {
            if (complete_signal_ != nullptr) {
                complete_signal_->pulse(config_.complete_pulse_duration);
            }
            if (const auto log = logger()) {
                log->info("capture saved: {}", saved_path);
            }
        } else if (const auto log = logger()) {
            log->error("capture failed: {}", error);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
        }
    }

    catcheye::runtime::ProcessOutput output;
    if (!context.needs_publish) {
        return output;
    }

    output.has_message = true;
    output.message.stream_name = "capture-viewer";
    output.message.metadata_json = status_json();
    output.publish_frame = frame;
    output.has_publish_frame = true;
    return output;
}

CaptureStatus CaptureProcessor::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        .trigger_gpio_enabled = config_.trigger_gpio.enabled,
        .complete_gpio_enabled = config_.complete_gpio.enabled,
        .busy = busy_,
        .capture_requested = capture_requested_,
        .capture_count = capture_count_,
        .ignored_trigger_count = ignored_trigger_count_,
        .last_saved_path = last_saved_path_,
        .last_error = last_error_,
    };
}

std::string CaptureProcessor::status_json() const
{
    return capture_status_json(status());
}

std::string CaptureProcessor::build_capture_path_locked()
{
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    const std::tm tm = local_time(now_time);

    std::ostringstream date_dir;
    date_dir << std::put_time(&tm, "%Y-%m-%d");

    const std::filesystem::path output_dir = std::filesystem::path(config_.capture_dir) / date_dir.str();
    std::filesystem::create_directories(output_dir);

    ++sequence_;
    std::ostringstream filename;
    filename << std::put_time(&tm, "%H%M%S")
             << '_' << std::setw(3) << std::setfill('0') << millis.count()
             << '_' << std::setw(6) << std::setfill('0') << sequence_
             << ".jpg";

    return std::filesystem::absolute(output_dir / filename.str()).string();
}

bool CaptureProcessor::save_frame_locked(const catcheye::input::Frame& frame, std::string& saved_path, std::string& error)
{
    const auto bgr = frame_to_bgr_mat(frame);
    if (bgr.empty()) {
        error = "frame cannot be converted to BGR";
        return false;
    }

    saved_path = build_capture_path_locked();
    const std::vector<int> params = {
        cv::IMWRITE_JPEG_QUALITY,
        config_.jpeg_quality,
    };
    if (!cv::imwrite(saved_path, bgr, params)) {
        error = "failed to write JPEG: " + saved_path;
        return false;
    }
    return true;
}

void CaptureProcessor::set_error_locked(std::string error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = std::move(error);
}

std::string capture_status_json(const CaptureStatus& status)
{
    std::ostringstream oss;
    oss << "{\"app\":\"catcheye-capture\""
        << ",\"kind\":\"capture\""
        << ",\"trigger_gpio_enabled\":" << (status.trigger_gpio_enabled ? "true" : "false")
        << ",\"complete_gpio_enabled\":" << (status.complete_gpio_enabled ? "true" : "false")
        << ",\"busy\":" << (status.busy ? "true" : "false")
        << ",\"capture_requested\":" << (status.capture_requested ? "true" : "false")
        << ",\"capture_count\":" << status.capture_count
        << ",\"ignored_trigger_count\":" << status.ignored_trigger_count
        << ",\"last_saved_path\":\"" << json_escape(status.last_saved_path) << "\""
        << ",\"last_error\":\"" << json_escape(status.last_error) << "\"}";
    return oss.str();
}

} // namespace catcheye::capture
