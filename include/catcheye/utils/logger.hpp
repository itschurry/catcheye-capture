#pragma once

#include <memory>
#include <string>

#include <spdlog/logger.h>

namespace catcheye {

bool initialize_logging(
    const std::string& logger_name = "catcheye",
    const std::string& log_directory = "log");

std::shared_ptr<spdlog::logger> logger();

void shutdown_logging();

} // namespace catcheye
