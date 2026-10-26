// H.264 decoding for the PSP movies with FFmpeg's software decoder (libavcodec, LGPL 2.1+).  Used by the browser build:
// browsers have no Media Foundation, and WebCodecs only hands frames over asynchronously.  The decoder is a trimmed
// copy of FFmpeg 7.1 (H.264 decoder + parser only, no threads, no assembly) in third_party/ffmpeg-h264.
// Movies are 480x272 Main profile, so software decoding costs ~1-2 ms a frame.
#include "rr_video.hpp"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
}

#include <algorithm>
#include <cstring>
#include <deque>

namespace rr {
namespace {

class FfmpegH264 final : public VideoDecoder {
public:
    bool open(int width, int height) {
        width_ = width; height_ = height;
        av_log_set_level(AV_LOG_ERROR);
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (codec == nullptr) return false;
        ctx_ = avcodec_alloc_context3(codec);
        if (ctx_ == nullptr) return false;
        ctx_->thread_count = 1;
        if (avcodec_open2(ctx_, codec, nullptr) < 0) return false;
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        return pkt_ != nullptr && frame_ != nullptr;
    }
    ~FfmpegH264() override {
        av_frame_free(&frame_);
        av_packet_free(&pkt_);
        avcodec_free_context(&ctx_);
    }

    bool push(const std::uint8_t *data, std::size_t size, std::uint64_t pts) override {
        // libavcodec reads up to AV_INPUT_BUFFER_PADDING_SIZE bytes past the end, so give it a zero-padded copy
        input_.assign(data, data + size);
        input_.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        pkt_->data = input_.data();
        pkt_->size = static_cast<int>(size);
        pkt_->pts = static_cast<std::int64_t>(pts);
        pkt_->dts = static_cast<std::int64_t>(pts);
        const int r = avcodec_send_packet(ctx_, pkt_);
        av_packet_unref(pkt_);
        receive();
        return r >= 0 || r == AVERROR(EAGAIN);
    }

    void drain() override {
        avcodec_send_packet(ctx_, nullptr);
        receive();
    }

    bool pop(std::vector<std::uint8_t> &rgba, int &width, int &height) override {
        if (frames_.empty()) return false;
        Frame &f = frames_.front();
        rgba.swap(f.rgba);
        width = f.w; height = f.h;
        frames_.pop_front();
        return true;
    }

private:
    struct Frame { std::vector<std::uint8_t> rgba; int w = 0, h = 0; };

    void receive() {
        while (avcodec_receive_frame(ctx_, frame_) == 0) {
            if (frame_->format == AV_PIX_FMT_YUV420P || frame_->format == AV_PIX_FMT_YUVJ420P) convert(*frame_);
            av_frame_unref(frame_);
        }
    }

    // BT.601 limited range, the same arithmetic as the Windows (Media Foundation) path
    void convert(const AVFrame &src) {
        const int w = std::min(src.width, width_), h = std::min(src.height, height_);
        Frame f;
        f.w = w; f.h = h;
        f.rgba.resize(static_cast<std::size_t>(w) * h * 4u);
        for (int y = 0; y < h; ++y) {
            const std::uint8_t *yr = src.data[0] + static_cast<std::ptrdiff_t>(y) * src.linesize[0];
            const std::uint8_t *ur = src.data[1] + static_cast<std::ptrdiff_t>(y / 2) * src.linesize[1];
            const std::uint8_t *vr = src.data[2] + static_cast<std::ptrdiff_t>(y / 2) * src.linesize[2];
            std::uint8_t *o = f.rgba.data() + static_cast<std::size_t>(y) * w * 4u;
            for (int x = 0; x < w; ++x) {
                const int c = (yr[x] - 16) * 298;
                const int d = ur[x >> 1] - 128, e = vr[x >> 1] - 128;
                const int r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8, b = (c + 516 * d + 128) >> 8;
                o[x * 4 + 0] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
                o[x * 4 + 1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
                o[x * 4 + 2] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
                o[x * 4 + 3] = 255;
            }
        }
        frames_.push_back(std::move(f));
    }

    AVCodecContext *ctx_ = nullptr;
    AVPacket *pkt_ = nullptr;
    AVFrame *frame_ = nullptr;
    int width_ = 480, height_ = 272;
    std::vector<std::uint8_t> input_;
    std::deque<Frame> frames_;
};

} // namespace

std::unique_ptr<VideoDecoder> create_h264_decoder(int width, int height) {
    auto d = std::make_unique<FfmpegH264>();
    if (!d->open(width, height)) return nullptr;
    return d;
}

} // namespace rr
