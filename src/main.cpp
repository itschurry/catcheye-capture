#include <exception>
#include <iostream>
#include <string_view>

#include "catcheye/utils/logger.hpp"
#include "capture/app.hpp"

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            return catcheye::capture::run_app(argc, argv);
        }
    }

    catcheye::initialize_logging("catcheye_capture", "log");
    try {
        const int exit_code = catcheye::capture::run_app(argc, argv);
        if (const auto log = catcheye::logger()) {
            log->info("catcheye-capture exiting with code {}", exit_code);
        }
        catcheye::shutdown_logging();
        return exit_code;
    } catch (const std::exception& exception) {
        if (const auto log = catcheye::logger()) {
            log->error("startup failed: {}", exception.what());
        } else {
            std::cerr << exception.what() << '\n';
        }
        catcheye::shutdown_logging();
        return 1;
    }
}
