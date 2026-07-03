#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "catcheye/hardware/gpio_signal_config.hpp"

namespace catcheye::capture {

class CaptureTriggerSignal {
  public:
    using Callback = std::function<void()>;

    virtual ~CaptureTriggerSignal() = default;
    virtual bool initialize(Callback callback) = 0;
    virtual void shutdown() = 0;
};

class CaptureCompleteSignal {
  public:
    virtual ~CaptureCompleteSignal() = default;
    virtual bool initialize() = 0;
    virtual void pulse(std::chrono::milliseconds duration) = 0;
    virtual void shutdown() = 0;
};

std::unique_ptr<CaptureTriggerSignal> make_gpio_trigger_signal(catcheye::GpioInputConfig config);
std::unique_ptr<CaptureCompleteSignal> make_gpio_complete_signal(catcheye::GpioSignalConfig config);

} // namespace catcheye::capture
