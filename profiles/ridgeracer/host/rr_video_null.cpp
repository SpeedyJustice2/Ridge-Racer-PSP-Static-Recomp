#include "rr_video.hpp"

namespace rr {
std::unique_ptr<VideoDecoder> create_h264_decoder(int, int) { return nullptr; }
}
