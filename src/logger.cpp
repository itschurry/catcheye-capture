#include "catcheye/utils/logger.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace catcheye {
namespace {

std::string build_log_filename(const std::string& logger_name, const std::string& log_directory) {
    const auto now = std::chrono::system_clock::now();
    const auto timestamp = std::chrono::system_clock::to_time_t(now);

    std::tm time_info{};
#if defined(_WIN32)
    localtime_s(&time_info, &timestamp);
#else
    localtime_r(&timestamp, &time_info);
#endif

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &time_info);
    return log_directory + "/" + logger_name + "_" + buffer + ".log";
}

std::shared_ptr<spdlog::logger>& global_logger() {
    static std::shared_ptr<spdlog::logger> instance;
    return instance;
}

} // namespace

bool initialize_logging(const std::string& logger_name, const std::string& log_directory) {
    if (global_logger()) {
        return true;
    }

    try {
        std::filesystem::create_directories(log_directory);

        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            build_log_filename(logger_name, log_directory),
            true);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");

        std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
        auto created_logger = std::make_shared<spdlog::logger>(
            logger_name,
            sinks.begin(),
            sinks.end());

#ifdef CATCHEYE_DEBUG
        created_logger->set_level(spdlog::level::debug);
#else
        created_logger->set_level(spdlog::level::info);
#endif
        created_logger->flush_on(spdlog::level::info);

        spdlog::set_default_logger(created_logger);
        global_logger() = std::move(created_logger);
        global_logger()->info("logging initialized at {}", log_directory);
        return true;
    } catch (const std::exception& exception) {
        std::cerr << "Failed to initialize logging: " << exception.what() << '\n';
        return false;
    }
}

std::shared_ptr<spdlog::logger> logger() {
    return global_logger();
}

void shutdown_logging() {
    if (global_logger()) {
        global_logger()->info("logging shutdown");
        global_logger()->flush();
        spdlog::drop(global_logger()->name());
        global_logger().reset();
    }
}

} // namespace catcheye
