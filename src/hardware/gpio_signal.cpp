#include "catcheye/hardware/gpio_signal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "catcheye/utils/logger.hpp"

#include <gpiod.h>

namespace catcheye::hardware {
namespace {

constexpr std::int64_t kInputPollTimeoutNs = 100'000'000;
constexpr unsigned int kEdgeEventBufferSize = 8;

std::chrono::milliseconds normalized_debounce(std::chrono::milliseconds duration) {
    return std::max(duration, std::chrono::milliseconds{0});
}

} // namespace

class GpioStateSignal::Backend {
  public:
    explicit Backend(GpioSignalConfig config)
        : config_(std::move(config)) {}

    bool initialize() {
        chip_ = gpiod_chip_open(config_.chip_path.c_str());
        if (chip_ == nullptr) {
            log_errno("failed to open gpio chip");
            return false;
        }

        offset_ = static_cast<unsigned int>(config_.line);

        gpiod_request_config* request_config = gpiod_request_config_new();
        if (request_config == nullptr) {
            log_errno("failed to create gpio request config");
            close_chip();
            return false;
        }
        gpiod_request_config_set_consumer(request_config, config_.consumer.c_str());

        gpiod_line_settings* settings = gpiod_line_settings_new();
        if (settings == nullptr) {
            log_errno("failed to create gpio line settings");
            gpiod_request_config_free(request_config);
            close_chip();
            return false;
        }

        if (gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT) < 0) {
            log_errno("failed to configure gpio line as output");
            gpiod_line_settings_free(settings);
            gpiod_request_config_free(request_config);
            close_chip();
            return false;
        }

        gpiod_line_settings_set_active_low(settings, config_.active_low);
        if (gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE) < 0) {
            log_errno("failed to configure initial gpio value");
            gpiod_line_settings_free(settings);
            gpiod_request_config_free(request_config);
            close_chip();
            return false;
        }

        gpiod_line_config* line_config = gpiod_line_config_new();
        if (line_config == nullptr) {
            log_errno("failed to create gpio line config");
            gpiod_line_settings_free(settings);
            gpiod_request_config_free(request_config);
            close_chip();
            return false;
        }

        if (gpiod_line_config_add_line_settings(line_config, &offset_, 1, settings) < 0) {
            log_errno("failed to add gpio line settings");
            gpiod_line_config_free(line_config);
            gpiod_line_settings_free(settings);
            gpiod_request_config_free(request_config);
            close_chip();
            return false;
        }

        request_ = gpiod_chip_request_lines(chip_, request_config, line_config);

        gpiod_line_config_free(line_config);
        gpiod_line_settings_free(settings);
        gpiod_request_config_free(request_config);

        if (request_ == nullptr) {
            log_errno("failed to request gpio line as output");
            close_chip();
            return false;
        }

        return true;
    }

    void set_active(bool active) {
        if (request_ == nullptr) {
            return;
        }

        const auto value = active ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
        if (gpiod_line_request_set_value(request_, offset_, value) < 0) {
            log_errno("failed to set gpio value");
        }
    }

    ~Backend() {
        close_line();
        close_chip();
    }

  private:
    void log_errno(const std::string& message) const {
        if (const auto log = logger()) {
            log->error("{}: {} (chip='{}', line={})", message, std::strerror(errno), config_.chip_path, config_.line);
        }
    }

    void close_line() {
        if (request_ != nullptr) {
            gpiod_line_request_release(request_);
            request_ = nullptr;
        }
    }

    void close_chip() {
        if (chip_ != nullptr) {
            gpiod_chip_close(chip_);
            chip_ = nullptr;
        }
    }

    gpiod_chip* chip_ = nullptr;
    gpiod_line_request* request_ = nullptr;
    unsigned int offset_ = 0;
    GpioSignalConfig config_;
};

GpioStateSignal::GpioStateSignal(GpioSignalConfig config)
    : config_(std::move(config)) {}

GpioStateSignal::~GpioStateSignal() {
    shutdown();
}

bool GpioStateSignal::initialize() {
    if (!config_.enabled || config_.line < 0) {
        return true;
    }

    backend_ = std::make_unique<Backend>(config_);
    if (!backend_->initialize()) {
        backend_.reset();
        return false;
    }

    initialized_ = true;

    if (const auto log = logger()) {
        log->info("GPIO state signal ready: chip='{}', line={}, active_low={}",
                  config_.chip_path,
                  config_.line,
                  config_.active_low);
    }
    return true;
}

void GpioStateSignal::set_state(bool active) {
    if (!initialized_ || backend_ == nullptr) {
        return;
    }

    set_active(active);
}

void GpioStateSignal::shutdown() {
    set_active(false);
    initialized_ = false;
    backend_.reset();
}

void GpioStateSignal::set_active(bool active) {
    if (backend_ != nullptr) {
        backend_->set_active(active);
    }
}

class GpioInputSignal::Backend {
  public:
    Backend(GpioInputConfig config, GpioInputSignal::StateCallback callback)
        : config_(std::move(config)),
          callback_(std::move(callback)) {}

    bool initialize() {
        chip_ = gpiod_chip_open(config_.chip_path.c_str());
        if (chip_ == nullptr) {
            log_errno("failed to open gpio chip");
            return false;
        }

        offset_ = static_cast<unsigned int>(config_.line);

        gpiod_request_config* request_config = gpiod_request_config_new();
        if (request_config == nullptr) {
            log_errno("failed to create gpio input request config");
            close_chip();
            return false;
        }
        gpiod_request_config_set_consumer(request_config, config_.consumer.c_str());

        gpiod_line_settings* settings = gpiod_line_settings_new();
        if (settings == nullptr) {
            log_errno("failed to create gpio input line settings");
            gpiod_request_config_free(request_config);
            close_chip();
            return false;
        }

        if (gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT) < 0) {
            log_errno("failed to configure gpio line as input");
            gpiod_line_settings_free(settings);
            gpiod_request_config_free(request_config);
            close_chip();
            return false;
        }
        if (gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH) < 0) {
            log_errno("failed to configure gpio input edge detection");
            gpiod_line_settings_free(settings);
            gpiod_request_config_free(request_config);
            close_chip();
            return false;
        }
        gpiod_line_settings_set_active_low(settings, config_.active_low);
        const auto debounce_us = std::chrono::duration_cast<std::chrono::microseconds>(normalized_debounce(config_.debounce_duration)).count();
        gpiod_line_settings_set_debounce_period_us(settings, static_cast<unsigned long>(debounce_us));

        gpiod_line_config* line_config = gpiod_line_config_new();
        if (line_config == nullptr) {
            log_errno("failed to create gpio input line config");
            gpiod_line_settings_free(settings);
            gpiod_request_config_free(request_config);
            close_chip();
            return false;
        }

        if (gpiod_line_config_add_line_settings(line_config, &offset_, 1, settings) < 0) {
            log_errno("failed to add gpio input line settings");
            gpiod_line_config_free(line_config);
            gpiod_line_settings_free(settings);
            gpiod_request_config_free(request_config);
            close_chip();
            return false;
        }

        request_ = gpiod_chip_request_lines(chip_, request_config, line_config);

        gpiod_line_config_free(line_config);
        gpiod_line_settings_free(settings);
        gpiod_request_config_free(request_config);

        if (request_ == nullptr) {
            log_errno("failed to request gpio line as input");
            close_chip();
            return false;
        }

        event_buffer_ = gpiod_edge_event_buffer_new(kEdgeEventBufferSize);
        if (event_buffer_ == nullptr) {
            log_errno("failed to create gpio edge event buffer");
            close_line();
            close_chip();
            return false;
        }

        const auto initial_state = read_active();
        if (!initial_state.has_value()) {
            close_event_buffer();
            close_line();
            close_chip();
            return false;
        }
        current_state_ = *initial_state;
        emit_state(current_state_);

        stop_requested_ = false;
        worker_ = std::thread(&Backend::worker_loop, this);
        return true;
    }

    void shutdown() {
        stop_requested_ = true;
        if (worker_.joinable()) {
            worker_.join();
        }
        close_event_buffer();
        close_line();
        close_chip();
    }

    ~Backend() {
        shutdown();
    }

  private:
    std::optional<bool> read_active() const {
        if (request_ == nullptr) {
            return std::nullopt;
        }
        const auto value = gpiod_line_request_get_value(request_, offset_);
        if (value < 0) {
            log_errno("failed to read gpio input value");
            return std::nullopt;
        }
        return value == GPIOD_LINE_VALUE_ACTIVE;
    }

    void worker_loop() {
        const auto debounce_duration = normalized_debounce(config_.debounce_duration);
        std::chrono::steady_clock::time_point last_emit_time;

        while (!stop_requested_) {
            const int wait_result = gpiod_line_request_wait_edge_events(request_, kInputPollTimeoutNs);
            if (wait_result < 0) {
                log_errno("failed to wait for gpio edge event");
                continue;
            }
            if (wait_result == 0) {
                continue;
            }

            const int event_count = gpiod_line_request_read_edge_events(request_, event_buffer_, kEdgeEventBufferSize);
            if (event_count < 0) {
                log_errno("failed to read gpio edge event");
                continue;
            }

            const auto state = read_active();
            if (!state.has_value() || *state == current_state_) {
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_emit_time < debounce_duration) {
                continue;
            }

            current_state_ = *state;
            last_emit_time = now;
            emit_state(current_state_);
        }
    }

    void emit_state(bool active) const {
        if (callback_) {
            callback_(active);
        }
    }

    void log_errno(const std::string& message) const {
        if (const auto log = logger()) {
            log->error("{}: {} (chip='{}', line={})", message, std::strerror(errno), config_.chip_path, config_.line);
        }
    }

    void close_event_buffer() {
        if (event_buffer_ != nullptr) {
            gpiod_edge_event_buffer_free(event_buffer_);
            event_buffer_ = nullptr;
        }
    }

    void close_line() {
        if (request_ != nullptr) {
            gpiod_line_request_release(request_);
            request_ = nullptr;
        }
    }

    void close_chip() {
        if (chip_ != nullptr) {
            gpiod_chip_close(chip_);
            chip_ = nullptr;
        }
    }

    gpiod_chip* chip_ = nullptr;
    gpiod_line_request* request_ = nullptr;
    gpiod_edge_event_buffer* event_buffer_ = nullptr;
    unsigned int offset_ = 0;
    bool current_state_ = false;
    std::atomic_bool stop_requested_ = false;
    std::thread worker_;
    GpioInputConfig config_;
    GpioInputSignal::StateCallback callback_;
};

GpioInputSignal::GpioInputSignal(GpioInputConfig config, StateCallback callback)
    : config_(std::move(config)),
      callback_(std::move(callback)) {}

GpioInputSignal::~GpioInputSignal() {
    shutdown();
}

bool GpioInputSignal::initialize() {
    if (!config_.enabled || config_.line < 0) {
        return true;
    }

    backend_ = std::make_unique<Backend>(config_, callback_);
    if (!backend_->initialize()) {
        backend_.reset();
        return false;
    }

    initialized_ = true;

    if (const auto log = logger()) {
        log->info("GPIO input signal ready: chip='{}', line={}, active_low={}, debounce_ms={}",
                  config_.chip_path,
                  config_.line,
                  config_.active_low,
                  normalized_debounce(config_.debounce_duration).count());
    }
    return true;
}

void GpioInputSignal::shutdown() {
    if (backend_ != nullptr) {
        backend_->shutdown();
    }
    initialized_ = false;
    backend_.reset();
}

} // namespace catcheye::hardware
