#pragma once

#include <opencv2/core/mat.hpp>

#include "catcheye/input/frame.hpp"

namespace catcheye::capture {

cv::Mat frame_to_bgr_mat(const catcheye::input::Frame& frame);

} // namespace catcheye::capture
