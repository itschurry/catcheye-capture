#include "capture/recording_controller.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <opencv2/videoio.hpp>

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

} // namespace

struct RecordingController::WriterState {
    cv::VideoWriter writer;
    cv::Size frame_size;
};

RecordingController::RecordingController(std::string output_dir, double fps)
    : output_dir_(std::move(output_dir)), fps_(fps)
{
    if (output_dir_.empty()) {
        throw std::runtime_error("recording output directory must not be empty");
    }
    if (fps_ <= 0.0) {
        throw std::runtime_error("recording fps must be positive");
    }
}

RecordingController::~RecordingController()
{
    std::lock_guard<std::mutex> lock(mutex_);
    close_writer_locked();
}

RecordingStatus RecordingController::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

RecordingStatus RecordingController::start()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != RecordingState::Idle) {
        status_.error = "recording is already active";
        return status_;
    }

    std::filesystem::create_directories(output_dir_);
    const std::string active_path = create_recording_path();
    if (std::filesystem::exists(active_path)) {
        status_.error = "recording path already exists: " + active_path;
        return status_;
    }

    status_.state = RecordingState::Recording;
    status_.active_path = active_path;
    status_.saved_path.clear();
    status_.error.clear();
    status_.written_frames = 0;
    writer_.reset();
    return status_;
}

RecordingStatus RecordingController::pause()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != RecordingState::Recording) {
        status_.error = "recording is not running";
        return status_;
    }
    status_.state = RecordingState::Paused;
    status_.error.clear();
    return status_;
}

RecordingStatus RecordingController::resume()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != RecordingState::Paused) {
        status_.error = "recording is not paused";
        return status_;
    }
    status_.state = RecordingState::Recording;
    status_.error.clear();
    return status_;
}

RecordingStatus RecordingController::save()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state == RecordingState::Idle) {
        status_.error = "recording is not active";
        return status_;
    }
    if (status_.written_frames == 0) {
        status_.error = "recording has no written frames";
        return status_;
    }
    close_writer_locked();
    if (!std::filesystem::exists(status_.active_path)) {
        status_.error = "recording file was not created";
        return status_;
    }
    status_.saved_path = status_.active_path;
    status_.active_path.clear();
    status_.state = RecordingState::Idle;
    status_.error.clear();
    return status_;
}

RecordingStatus RecordingController::cancel()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state == RecordingState::Idle) {
        status_.error = "recording is not active";
        return status_;
    }
    close_writer_locked();
    remove_active_file_locked();
    status_.active_path.clear();
    status_.saved_path.clear();
    status_.state = RecordingState::Idle;
    status_.error.clear();
    status_.written_frames = 0;
    return status_;
}

void RecordingController::write_frame(const catcheye::input::Frame& frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != RecordingState::Recording) {
        return;
    }
    if (!writer_ && !open_writer_locked(frame)) {
        return;
    }

    const cv::Mat bgr = frame_to_bgr_mat(frame);
    if (bgr.empty()) {
        status_.error = "preview frame cannot be converted to BGR";
        return;
    }
    if (bgr.size() != writer_->frame_size) {
        status_.error = "preview frame size changed during recording";
        return;
    }
    writer_->writer.write(bgr);
    ++status_.written_frames;
}

void RecordingController::close_writer_locked()
{
    if (writer_) {
        writer_->writer.release();
        writer_.reset();
    }
}

void RecordingController::remove_active_file_locked()
{
    if (!status_.active_path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(status_.active_path, ignored);
    }
}

bool RecordingController::open_writer_locked(const catcheye::input::Frame& frame)
{
    const cv::Mat bgr = frame_to_bgr_mat(frame);
    if (bgr.empty()) {
        status_.error = "preview frame cannot be converted to BGR";
        return false;
    }

    auto next_writer = std::make_unique<WriterState>();
    next_writer->frame_size = bgr.size();
    const int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
    if (!next_writer->writer.open(status_.active_path, fourcc, fps_, next_writer->frame_size, true)) {
        status_.error = "failed to open recording file: " + status_.active_path;
        return false;
    }
    writer_ = std::move(next_writer);
    return true;
}

std::string RecordingController::create_recording_path() const
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&now_time, &tm);

    std::ostringstream filename;
    filename << "catcheye_capture_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".mp4";
    return std::filesystem::absolute(std::filesystem::path(output_dir_) / filename.str()).string();
}

const char* recording_state_name(RecordingState state)
{
    switch (state) {
        case RecordingState::Idle:
            return "idle";
        case RecordingState::Recording:
            return "recording";
        case RecordingState::Paused:
            return "paused";
    }
    return "idle";
}

std::string recording_status_json(const RecordingStatus& status)
{
    std::ostringstream oss;
    oss << "{\"state\":\"" << recording_state_name(status.state) << "\""
        << ",\"active_path\":\"" << json_escape(status.active_path) << "\""
        << ",\"saved_path\":\"" << json_escape(status.saved_path) << "\""
        << ",\"error\":\"" << json_escape(status.error) << "\""
        << ",\"written_frames\":" << status.written_frames << "}";
    return oss.str();
}

} // namespace catcheye::capture
