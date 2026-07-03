#pragma once

#include <chrono>
#include <string>

namespace catcheye {

struct GpioSignalConfig {
    bool enabled = false;
    std::string chip_path = "/dev/gpiochip4";
    int line = -1;
    bool active_low = false;
    std::string consumer = "catcheye-capture";
};

struct GpioInputConfig {
    bool enabled = false;
    std::string chip_path = "/dev/gpiochip4";
    int line = -1;
    bool active_low = false;
    std::chrono::milliseconds debounce_duration{200};
    std::string consumer = "catcheye-capture";
};

} // namespace catcheye
