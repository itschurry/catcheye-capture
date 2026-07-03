#include "capture/signal.hpp"

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "catcheye/hardware/gpio_signal.hpp"

namespace catcheye::capture {
namespace {

class GpioTriggerSignal final : public CaptureTriggerSignal {
  public:
    explicit GpioTriggerSignal(catcheye::GpioInputConfig config)
        : config_(std::move(config)) {}

    bool initialize(Callback callback) override
    {
        callback_ = std::move(callback);
        initial_state_seen_ = false;
        input_ = std::make_unique<catcheye::hardware::GpioInputSignal>(
            config_,
            [this](bool active) {
                if (!initial_state_seen_) {
                    initial_state_seen_ = true;
                    return;
                }
                if (active && callback_) {
                    callback_();
                }
            });
        if (!input_->initialize()) {
            input_.reset();
            return false;
        }
        return true;
    }

    void shutdown() override
    {
        if (input_ != nullptr) {
            input_->shutdown();
            input_.reset();
        }
    }

  private:
    catcheye::GpioInputConfig config_;
    Callback callback_;
    std::unique_ptr<catcheye::hardware::GpioInputSignal> input_;
    bool initial_state_seen_ = false;
};

class GpioCompleteSignal final : public CaptureCompleteSignal {
  public:
    explicit GpioCompleteSignal(catcheye::GpioSignalConfig config)
        : output_(std::move(config)) {}

    bool initialize() override
    {
        return output_.initialize();
    }

    void pulse(std::chrono::milliseconds duration) override
    {
        output_.set_state(true);
        std::this_thread::sleep_for(duration);
        output_.set_state(false);
    }

    void shutdown() override
    {
        output_.shutdown();
    }

  private:
    catcheye::hardware::GpioStateSignal output_;
};

} // namespace

std::unique_ptr<CaptureTriggerSignal> make_gpio_trigger_signal(catcheye::GpioInputConfig config)
{
    return std::make_unique<GpioTriggerSignal>(std::move(config));
}

std::unique_ptr<CaptureCompleteSignal> make_gpio_complete_signal(catcheye::GpioSignalConfig config)
{
    return std::make_unique<GpioCompleteSignal>(std::move(config));
}

} // namespace catcheye::capture
