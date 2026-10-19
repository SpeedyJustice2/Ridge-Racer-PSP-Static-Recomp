#include "rr_video.hpp"

#include "rr_log.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>

namespace rr {
namespace {

// GUIDs spelled out so the build needs nothing beyond mfplat.lib / ole32.lib.
const GUID kClsidH264Decoder = {0x62CE7E72, 0x4C71, 0x4d20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};
const GUID kMediaTypeVideo = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kFormatH264 = {0x34363248, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kFormatNV12 = {0x3231564E, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

template <class T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

class MfH264Decoder final : public VideoDecoder {
public:
    ~MfH264Decoder() override {
        release(mft_);
        if (mf_started_) MFShutdown();
    }

    bool open(int width, int height) {
        width_ = width; height_ = height;
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        (void)co;   // S_FALSE / RPC_E_CHANGED_MODE: COM is already usable on this thread
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return fail("MFStartup");
        mf_started_ = true;
        if (FAILED(CoCreateInstance(kClsidH264Decoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_)))) return fail("create H.264 decoder");
        IMFMediaType *in = nullptr;
        if (FAILED(MFCreateMediaType(&in))) return fail("MFCreateMediaType");
        in->SetGUID(MF_MT_MAJOR_TYPE, kMediaTypeVideo);
        in->SetGUID(MF_MT_SUBTYPE, kFormatH264);
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, static_cast<UINT32>(width), static_cast<UINT32>(height));
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        const HRESULT hr = mft_->SetInputType(0, in, 0);
        release(in);
        if (FAILED(hr)) return fail("SetInputType");
        if (!select_output()) return false;
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool push(const std::uint8_t *data, std::size_t size, std::uint64_t pts) override {
        if (mft_ == nullptr || size == 0) return false;
        IMFMediaBuffer *buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &buf))) return false;
        BYTE *dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr))) { release(buf); return false; }
        std::memcpy(dst, data, size);
        buf->Unlock();
        buf->SetCurrentLength(static_cast<DWORD>(size));
        IMFSample *sample = nullptr;
        if (FAILED(MFCreateSample(&sample))) { release(buf); return false; }
        sample->AddBuffer(buf);
        sample->SetSampleTime(static_cast<LONGLONG>(pts) * 10000000ll / 90000ll);
        release(buf);
        HRESULT hr = mft_->ProcessInput(0, sample, 0);
        if (hr == MF_E_NOTACCEPTING) {            // output is backed up: collect it and retry
            collect();
            hr = mft_->ProcessInput(0, sample, 0);
        }
        release(sample);
        if (FAILED(hr)) { if (++input_errors_ <= 3) log_line(category::kHle, "movie: H.264 ProcessInput failed hr=0x" + hex(hr)); return false; }
        collect();
        return true;
    }

    void drain() override {
        if (mft_ == nullptr) return;
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        collect();
    }

    bool pop(std::vector<std::uint8_t> &rgba, int &width, int &height) override {
        if (frames_.empty()) return false;
        rgba = std::move(frames_.front().rgba);
        width = frames_.front().w; height = frames_.front().h;
        frames_.pop_front();
        return true;
    }

private:
    struct Frame { std::vector<std::uint8_t> rgba; int w, h; };

    static std::string hex(HRESULT hr) {
        char b[16]; std::snprintf(b, sizeof b, "%08X", static_cast<unsigned>(hr)); return b;
    }
    bool fail(const char *what) { log_line(category::kHle, std::string("movie: H.264 decoder unavailable (") + what + ")"); return false; }

    bool select_output() {
        for (DWORD i = 0;; ++i) {
            IMFMediaType *t = nullptr;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) break;
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == kFormatNV12) {
                const HRESULT hr = mft_->SetOutputType(0, t, 0);
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
                if (w != 0 && h != 0) { out_w_ = static_cast<int>(w); out_h_ = static_cast<int>(h); }
                LONG stride = 0;
                UINT32 s = 0;
                if (SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = static_cast<LONG>(static_cast<INT32>(s));
                out_stride_ = stride > 0 ? static_cast<int>(stride) : out_w_;
                release(t);
                if (SUCCEEDED(hr)) return true;
                return fail("SetOutputType");
            }
            release(t);
        }
        return fail("no NV12 output");
    }

    void collect() {
        for (int guard = 0; guard < 64; ++guard) {
            MFT_OUTPUT_STREAM_INFO info{};
            mft_->GetOutputStreamInfo(0, &info);
            IMFSample *out = nullptr;
            IMFMediaBuffer *buf = nullptr;
            MFT_OUTPUT_DATA_BUFFER data{};
            if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                if (FAILED(MFCreateSample(&out)) || FAILED(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, static_cast<DWORD>(out_stride_ * out_h_ * 3 / 2 + 4096)), &buf))) { release(out); release(buf); return; }
                out->AddBuffer(buf);
                data.pSample = out;
            }
            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &data, &status);
            release(buf);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { release(out); if (data.pEvents) data.pEvents->Release(); return; }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { release(out); if (data.pEvents) data.pEvents->Release(); select_output(); continue; }
            if (FAILED(hr)) { release(out); if (data.pEvents) data.pEvents->Release(); return; }
            IMFSample *got = data.pSample;
            if (got != nullptr) convert(got);
            if (data.pEvents) data.pEvents->Release();
            if (got != nullptr && got != out) got->Release();   // a sample the decoder allocated itself
            release(out);
        }
    }

    void convert(IMFSample *sample) {
        IMFMediaBuffer *buf = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return;
        BYTE *p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
            const int w = std::min(out_w_, width_), h = std::min(out_h_, height_);
            const int stride = out_stride_;
            const std::uint8_t *yp = p, *uv = p + static_cast<std::size_t>(stride) * out_h_;
            if (static_cast<std::size_t>(stride) * out_h_ * 3 / 2 <= len) {
                Frame f;
                f.w = w; f.h = h;
                f.rgba.resize(static_cast<std::size_t>(w) * h * 4u);
                for (int y = 0; y < h; ++y) {
                    const std::uint8_t *yr = yp + static_cast<std::size_t>(y) * stride;
                    const std::uint8_t *cr = uv + static_cast<std::size_t>(y / 2) * stride;
                    std::uint8_t *o = f.rgba.data() + static_cast<std::size_t>(y) * w * 4u;
                    for (int x = 0; x < w; ++x) {   // BT.601, limited range
                        const int c = (yr[x] - 16) * 298;
                        const int d = cr[(x & ~1)] - 128, e = cr[(x & ~1) + 1] - 128;
                        const int r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8, b = (c + 516 * d + 128) >> 8;
                        o[x * 4 + 0] = static_cast<std::uint8_t>(std::clamp(r, 0, 255));
                        o[x * 4 + 1] = static_cast<std::uint8_t>(std::clamp(g, 0, 255));
                        o[x * 4 + 2] = static_cast<std::uint8_t>(std::clamp(b, 0, 255));
                        o[x * 4 + 3] = 255;
                    }
                }
                frames_.push_back(std::move(f));
            }
            buf->Unlock();
        }
        release(buf);
    }

    IMFTransform *mft_ = nullptr;
    bool mf_started_ = false;
    int width_ = 480, height_ = 272, out_w_ = 480, out_h_ = 272, out_stride_ = 480;
    int input_errors_ = 0;
    std::deque<Frame> frames_;
};

} // namespace

std::unique_ptr<VideoDecoder> create_h264_decoder(int width, int height) {
    auto d = std::make_unique<MfH264Decoder>();
    if (!d->open(width, height)) return nullptr;
    return d;
}

} // namespace rr
