cmake_minimum_required(VERSION 3.20)

project(catcheye-capture
    VERSION 0.1.0
    DESCRIPTION "CatchEye Capture"
    LANGUAGES CXX
)

option(CATCHEYE_CAPTURE_BUILD_TESTS "Build tests" OFF)
option(CATCHEYE_CAPTURE_BUILD_APP "Build main executable if src/main.cpp exists" ON)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CATCHEYE_CAPTURE_HARDWARE_DEFAULT ON)
else()
    set(CATCHEYE_CAPTURE_HARDWARE_DEFAULT OFF)
endif()

option(CATCHEYE_CAPTURE_ENABLE_LIBCAMERA "Build Raspberry Pi libcamera input backend" ${CATCHEYE_CAPTURE_HARDWARE_DEFAULT})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
elseif(MSVC)
    add_compile_options(/W4)
endif()

set(CATCHEYE_VISION_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/catcheye-vision-sdk")
if(NOT EXISTS "${CATCHEYE_VISION_SDK_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "catcheye-vision-sdk repo not found at ${CATCHEYE_VISION_SDK_SOURCE_DIR}")
endif()

set(CATCHEYE_VISION_SDK_BUILD_TESTS OFF CACHE BOOL "Build tests for SDK libraries" FORCE)
set(CATCHEYE_VISION_INPUT_ENABLE_LIBCAMERA ${CATCHEYE_CAPTURE_ENABLE_LIBCAMERA} CACHE BOOL "Build libcamera input backend" FORCE)
set(CATCHEYE_VISION_INPUT_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(CATCHEYE_VISION_ROI_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(CATCHEYE_VISION_RUNTIME_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(CATCHEYE_VISION_HTTP_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)

find_package(OpenMP REQUIRED COMPONENTS CXX)

add_subdirectory(
    "${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-protocol"
    "${CMAKE_CURRENT_BINARY_DIR}/catcheye-vision-sdk/vision-protocol"
)
add_subdirectory(
    "${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-input"
    "${CMAKE_CURRENT_BINARY_DIR}/catcheye-vision-sdk/vision-input"
)
add_subdirectory(
    "${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-transport"
    "${CMAKE_CURRENT_BINARY_DIR}/catcheye-vision-sdk/vision-transport"
)
add_subdirectory(
    "${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-runtime"
    "${CMAKE_CURRENT_BINARY_DIR}/catcheye-vision-sdk/vision-runtime"
)
add_subdirectory(
    "${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-roi"
    "${CMAKE_CURRENT_BINARY_DIR}/catcheye-vision-sdk/vision-roi"
)
add_subdirectory(
    "${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-http"
    "${CMAKE_CURRENT_BINARY_DIR}/catcheye-vision-sdk/vision-http"
)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
message(STATUS "OpenCV version: ${OpenCV_VERSION}")

find_package(spdlog REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod>=2.0)
message(STATUS "libgpiod version: ${GPIOD_VERSION}")

set(CATCHEYE_CAPTURE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/hardware/gpio_signal.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capture/app.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capture/camera_properties.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capture/frame_conversion.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capture/http_api_server.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capture/processor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capture/recording_controller.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/capture/signal.cpp"
)

set(CATCHEYE_CAPTURE_MAIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(catcheye_capture STATIC ${CATCHEYE_CAPTURE_SOURCES})
add_library(catcheye::capture ALIAS catcheye_capture)

target_include_directories(catcheye_capture
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${OpenCV_INCLUDE_DIRS}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(catcheye_capture PUBLIC cxx_std_20)

target_compile_definitions(catcheye_capture
    PUBLIC
        $<$<CONFIG:Debug>:CATCHEYE_DEBUG>
)

target_link_libraries(catcheye_capture
    PUBLIC
        catcheye::vision_input
        catcheye::vision_protocol
        catcheye::vision_runtime
        catcheye::vision_transport
        catcheye::vision_http
        ${OpenCV_LIBS}
        spdlog::spdlog
        PkgConfig::GPIOD
)

set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

if(CATCHEYE_CAPTURE_BUILD_APP AND EXISTS "${CATCHEYE_CAPTURE_MAIN_SOURCE}")
    add_executable(catcheye-capture "${CATCHEYE_CAPTURE_MAIN_SOURCE}")

    target_link_libraries(catcheye-capture
        PRIVATE
            catcheye::capture
    )

    target_include_directories(catcheye-capture
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_features(catcheye-capture PRIVATE cxx_std_20)
endif()

if(CATCHEYE_CAPTURE_BUILD_TESTS)
    include(CTest)
    enable_testing()

    file(GLOB_RECURSE CATCHEYE_CAPTURE_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    )

    if(CATCHEYE_CAPTURE_TEST_SOURCES)
        add_executable(catcheye_capture_tests ${CATCHEYE_CAPTURE_TEST_SOURCES})

        target_link_libraries(catcheye_capture_tests
            PRIVATE
                catcheye::capture
        )

        target_include_directories(catcheye_capture_tests
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

        add_test(
            NAME catcheye_capture_tests
            COMMAND catcheye_capture_tests
        )
    endif()
endif()

include(GNUInstallDirs)

set(CATCHEYE_CAPTURE_LAUNCHER "${CMAKE_CURRENT_BINARY_DIR}/catcheye-capture")

if(TARGET catcheye-capture)
    set_target_properties(catcheye-capture PROPERTIES OUTPUT_NAME "catcheye-capture-bin")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/catcheye-capture-launcher.sh.in"
        "${CATCHEYE_CAPTURE_LAUNCHER}"
        @ONLY
    )
endif()

install(TARGETS catcheye_vision_input
    EXPORT catcheye_capture_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS catcheye_vision_roi
    EXPORT catcheye_capture_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS catcheye_vision_protocol
    EXPORT catcheye_capture_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS catcheye_vision_transport
    EXPORT catcheye_capture_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS catcheye_vision_runtime
    EXPORT catcheye_capture_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS catcheye_vision_http
    EXPORT catcheye_capture_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS catcheye_capture
    EXPORT catcheye_capture_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(TARGET catcheye-capture)
    install(TARGETS catcheye-capture
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(PROGRAMS "${CATCHEYE_CAPTURE_LAUNCHER}"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME catcheye-capture
    )
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(CATCHEYE_CAPTURE_ENABLE_LIBCAMERA)
    install(DIRECTORY ${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-input/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
else()
    install(DIRECTORY ${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-input/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "libcamera_source.hpp" EXCLUDE
    )
endif()

install(DIRECTORY ${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-protocol/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-roi/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-transport/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-runtime/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CATCHEYE_VISION_SDK_SOURCE_DIR}/libs/vision-http/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PATTERN "roi_api.hpp" EXCLUDE
)

install(PROGRAMS
    scripts/run.sh
    DESTINATION scripts
)

install(
    DIRECTORY config
    DESTINATION ${CMAKE_INSTALL_DATADIR}/..
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
)

install(FILES scripts/catcheye-capture.service
    DESTINATION scripts
)

install(EXPORT catcheye_capture_targets
    FILE catcheye_capture_targets.cmake
    NAMESPACE catcheye::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/catcheye_capture
)
