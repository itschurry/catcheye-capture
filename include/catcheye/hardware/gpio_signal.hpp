#pragma once

#include <functional>
#include <memory>
#include <string>

#include "catcheye/hardware/gpio_signal_config.hpp"

namespace catcheye::hardware {

class GpioStateSignal {
  public:
    explicit GpioStateSignal(GpioSignalConfig config);
    ~GpioStateSignal();

    GpioStateSignal(const GpioStateSignal&) = delete;
    GpioStateSignal& operator=(const GpioStateSignal&) = delete;

    bool initialize();
    void set_state(bool active);
    void shutdown();

  private:
    class Backend;

    void set_active(bool active);

    GpioSignalConfig config_;
    std::unique_ptr<Backend> backend_;
    bool initialized_ = false;
};

class GpioInputSignal {
  public:
    using StateCallback = std::function<void(bool active)>;

    GpioInputSignal(GpioInputConfig config, StateCallback callback);
    ~GpioInputSignal();

    GpioInputSignal(const GpioInputSignal&) = delete;
    GpioInputSignal& operator=(const GpioInputSignal&) = delete;

    bool initialize();
    void shutdown();

  private:
    class Backend;

    GpioInputConfig config_;
    StateCallback callback_;
    std::unique_ptr<Backend> backend_;
    bool initialized_ = false;
};

} // namespace catcheye::hardware
