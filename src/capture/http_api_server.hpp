#pragma once

#include <memory>
#include <string>

#include "catcheye/http/http_server.hpp"
#include "catcheye/input/frame_source.hpp"

namespace catcheye::capture {

class CaptureProcessor;

struct HttpApiServerConfig {
    std::string bind_address = "0.0.0.0";
    int port = 8090;
};

class HttpApiServer {
  public:
    HttpApiServer(
        HttpApiServerConfig config,
        CaptureProcessor* processor,
        catcheye::input::FrameSource* camera_source,
        std::string camera_properties_path);
    ~HttpApiServer();

    bool start();
    void stop();

  private:
    HttpApiServerConfig config_;
    std::string camera_properties_path_;
    CaptureProcessor* processor_ = nullptr;
    catcheye::input::FrameSource* camera_source_ = nullptr;
    std::unique_ptr<catcheye::http::HttpServer> server_;
};

} // namespace catcheye::capture
