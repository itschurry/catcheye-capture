#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "catcheye/input/frame.hpp"

namespace catcheye::capture {

enum class RecordingState {
    Idle,
    Recording,
    Paused,
};

struct RecordingStatus {
    RecordingState state = RecordingState::Idle;
    std::string active_path;
    std::string saved_path;
    std::string error;
    std::uint64_t written_frames = 0;
};

class RecordingController {
  public:
    explicit RecordingController(std::string output_dir = "recordings", double fps = 15.0);
    ~RecordingController();

    RecordingStatus status() const;
    RecordingStatus start();
    RecordingStatus pause();
    RecordingStatus resume();
    RecordingStatus save();
    RecordingStatus cancel();
    void write_frame(const catcheye::input::Frame& frame);

  private:
    void close_writer_locked();
    void remove_active_file_locked();
    bool open_writer_locked(const catcheye::input::Frame& frame);
    std::string create_recording_path() const;

    mutable std::mutex mutex_;
    std::string output_dir_;
    double fps_ = 15.0;
    RecordingStatus status_;
    struct WriterState;
    std::unique_ptr<WriterState> writer_;
};

const char* recording_state_name(RecordingState state);
std::string recording_status_json(const RecordingStatus& status);

} // namespace catcheye::capture
