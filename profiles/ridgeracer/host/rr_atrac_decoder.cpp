#include "rr_atrac_decoder.hpp"

#include "rr_log.hpp"
#include "rr_atrac3p.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


namespace rr {
namespace {

// ATRAC3+ decoding with the decoder source built into the port (third_party/atrac3plus, from FFmpeg's LGPL files).
class BuiltinAtrac3pDecoder final : public AtracDecoder {
public:
    ~BuiltinAtrac3pDecoder() override { rr_at3p_close(dec_); }

    bool open(int channels, int block_align) {
        dec_ = rr_at3p_open(channels, block_align);
        if (dec_ == nullptr) { log_line(category::kHle, "atrac: built-in ATRAC3+ decoder could not start"); return false; }
        channels_ = channels;
        for (int c = 0; c < channels; ++c) { planes_[c].assign(2048, 0.0f); ptrs_[c] = planes_[c].data(); }
        return true;
    }

    int decode(const std::uint8_t *data, std::size_t size, std::int16_t *out, int max_frames) override {
        if (dec_ == nullptr || size == 0) return 0;
        const int rc = rr_at3p_decode(dec_, data, static_cast<int>(size), ptrs_);
        if (rc < 0) { if (++errors_ <= 5) log_line(category::kHle, "atrac: built-in decode rc=" + std::to_string(rc)); return 0; }
        const int n = std::min(rc, max_frames);
        for (int c = 0; c < channels_; ++c)
            for (int i = 0; i < n; ++i)
                out[i * channels_ + c] = static_cast<std::int16_t>(std::clamp(planes_[c][i] * 32768.0f, -32768.0f, 32767.0f));
        return n;
    }

    // FFmpeg's ATRAC3+ decoder has no flush callback either, so the overlap state is kept (same behaviour as before).
    void flush() override {}
    int channels() const override { return channels_; }

private:
    RrAt3p *dec_ = nullptr;
    std::vector<float> planes_[8];
    float *ptrs_[8] = {};
    int channels_ = 2;
    int errors_ = 0;
};

} // namespace
} // namespace rr

#ifdef RR_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

namespace rr {
namespace {

class FfmpegAtracDecoder final : public AtracDecoder {
public:
    ~FfmpegAtracDecoder() override {
        av_frame_free(&frame_);
        av_packet_free(&packet_);
        avcodec_free_context(&ctx_);
    }

    bool open(bool plus, int channels, int rate, int block_align, const std::uint8_t *extra, std::size_t extra_size) {
        const AVCodec *codec = avcodec_find_decoder(plus ? AV_CODEC_ID_ATRAC3P : AV_CODEC_ID_ATRAC3);
        if (codec == nullptr) { log_line(category::kHle, "atrac: FFmpeg has no ATRAC decoder"); return false; }
        ctx_ = avcodec_alloc_context3(codec);
        if (ctx_ == nullptr) return false;
        channels_ = channels;
        ctx_->sample_rate = rate;
        ctx_->block_align = block_align;
#if LIBAVCODEC_VERSION_MAJOR >= 59
        av_channel_layout_default(&ctx_->ch_layout, channels);
#else
        ctx_->channels = channels;
        ctx_->channel_layout = channels == 1 ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
#endif
        if (extra != nullptr && extra_size != 0) {
            ctx_->extradata = static_cast<std::uint8_t *>(av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE));
            std::memcpy(ctx_->extradata, extra, extra_size);
            ctx_->extradata_size = static_cast<int>(extra_size);
        }
        const int rc = avcodec_open2(ctx_, codec, nullptr);
        if (rc < 0) { log_line(category::kHle, "atrac: avcodec_open2 failed rc=" + std::to_string(rc)); return false; }
        packet_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        return packet_ != nullptr && frame_ != nullptr;
    }

    int decode(const std::uint8_t *data, std::size_t size, std::int16_t *out, int max_frames) override {
        if (ctx_ == nullptr || size == 0) return 0;
        if (av_new_packet(packet_, static_cast<int>(size)) < 0) return 0;
        std::memcpy(packet_->data, data, size);
        const int sent = avcodec_send_packet(ctx_, packet_);
        av_packet_unref(packet_);
        if (sent < 0) { if (++errors_ <= 5) log_line(category::kHle, "atrac: send_packet rc=" + std::to_string(sent)); return 0; }
        const int got = avcodec_receive_frame(ctx_, frame_);
        if (got < 0) { if (++errors_ <= 5) log_line(category::kHle, "atrac: receive_frame rc=" + std::to_string(got)); return 0; }
        const int n = std::min(frame_->nb_samples, max_frames);
        const int ch = channels_;
        for (int c = 0; c < ch; ++c) {
            for (int i = 0; i < n; ++i) {
                float v = 0.0f;
                switch (frame_->format) {
                case AV_SAMPLE_FMT_FLTP: v = reinterpret_cast<const float *>(frame_->extended_data[c])[i]; break;
                case AV_SAMPLE_FMT_FLT: v = reinterpret_cast<const float *>(frame_->extended_data[0])[i * ch + c]; break;
                case AV_SAMPLE_FMT_S16P: out[i * ch + c] = reinterpret_cast<const std::int16_t *>(frame_->extended_data[c])[i]; continue;
                case AV_SAMPLE_FMT_S16: out[i * ch + c] = reinterpret_cast<const std::int16_t *>(frame_->extended_data[0])[i * ch + c]; continue;
                default: break;
                }
                out[i * ch + c] = static_cast<std::int16_t>(std::clamp(v * 32768.0f, -32768.0f, 32767.0f));
            }
        }
        av_frame_unref(frame_);
        return n;
    }

    void flush() override { if (ctx_ != nullptr) avcodec_flush_buffers(ctx_); }
    int channels() const override { return channels_; }

private:
    AVCodecContext *ctx_ = nullptr;
    AVPacket *packet_ = nullptr;
    AVFrame *frame_ = nullptr;
    int channels_ = 2;
    int errors_ = 0;
};

} // namespace

// Developer check (needs a build with FFmpeg and --dev): runs both decoders and logs how far apart the samples are.
class CompareAtracDecoder final : public AtracDecoder {
public:
    CompareAtracDecoder(std::unique_ptr<AtracDecoder> a, std::unique_ptr<AtracDecoder> b) : a_(std::move(a)), b_(std::move(b)) {}
    int decode(const std::uint8_t *data, std::size_t size, std::int16_t *out, int max_frames) override {
        const int na = a_->decode(data, size, out, max_frames);
        tmp_.resize(static_cast<std::size_t>(max_frames) * a_->channels());
        const int nb = b_->decode(data, size, tmp_.data(), max_frames);
        ++frames_;
        if (na != nb) ++count_mismatch_;
        const int n = std::min(na, nb) * a_->channels();
        for (int i = 0; i < n; ++i) max_diff_ = std::max(max_diff_, std::abs(static_cast<int>(out[i]) - static_cast<int>(tmp_[i])));
        if (frames_ == 1 || frames_ % 500 == 0)
            log_line(category::kHle, "atrac compare: frames=" + std::to_string(frames_) + " length_mismatches=" + std::to_string(count_mismatch_) +
                                         " max_sample_diff=" + std::to_string(max_diff_) + " (16-bit steps; 0..2 expected)");
        return na;
    }
    void flush() override { a_->flush(); b_->flush(); }
    int channels() const override { return a_->channels(); }
private:
    std::unique_ptr<AtracDecoder> a_, b_;
    std::vector<std::int16_t> tmp_;
    int frames_ = 0, count_mismatch_ = 0, max_diff_ = 0;
};

std::unique_ptr<AtracDecoder> create_atrac_decoder(bool plus, int channels, int sample_rate, int block_align,
                                                   const std::uint8_t *extradata, std::size_t extradata_size) {
    auto ff = std::make_unique<FfmpegAtracDecoder>();
    const bool ff_ok = ff->open(plus, channels, sample_rate, block_align, extradata, extradata_size);
    if (plus) {
        auto b = std::make_unique<BuiltinAtrac3pDecoder>();
        if (b->open(channels, block_align)) {
            if (ff_ok && std::getenv("RR_ATRAC_COMPARE") != nullptr)
                return std::make_unique<CompareAtracDecoder>(std::move(b), std::move(ff));
            if (ff_ok && std::getenv("RR_ATRAC_FFMPEG") != nullptr) return ff;
            return b;
        }
    }
    if (ff_ok) return ff;
    return nullptr;
}

} // namespace rr
#else
namespace rr {
// Without FFmpeg only ATRAC3+ (the game's music format) is decoded; plain ATRAC3 stays silent.
std::unique_ptr<AtracDecoder> create_atrac_decoder(bool plus, int channels, int, int block_align, const std::uint8_t *, std::size_t) {
    if (!plus) return nullptr;
    auto b = std::make_unique<BuiltinAtrac3pDecoder>();
    if (!b->open(channels, block_align)) return nullptr;
    return b;
}
}
#endif
