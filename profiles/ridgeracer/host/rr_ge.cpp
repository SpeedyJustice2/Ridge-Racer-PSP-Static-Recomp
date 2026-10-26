#include <optional>
#include <cstdio>
#include "rr_ge.hpp"
#include "rr_crumb.hpp"
#include "rr_glyph_patch.hpp"

#include "psprecomp/guest_memory.hpp"
#include "rr_log.hpp"
#include "rr_prof.hpp"

#include <algorithm>
#include <unordered_set>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

namespace rr {

namespace {

constexpr std::uint32_t kVram = 0x04000000u;

// The GE addresses EDRAM either with the 0x04000000 prefix or as a bare offset.
std::uint32_t gpu_address(std::uint32_t address) {
    address &= 0x0FFFFFFFu;
    if ((address & 0x0F000000u) == 0u) address |= kVram;
    return address;
}

std::uint32_t canon(std::uint32_t a) { return a & 0x1FFFFFFFu; }

float float_from_data(std::uint32_t data) {
    const std::uint32_t bits = data << 8u;
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool cull_disabled() {
    static const bool disabled = [] {
        const char *text = std::getenv("PSPRECOMP_RIDGERACER_CULL");
        return text != nullptr && std::string(text) == "off";
    }();
    return disabled;
}

} // namespace

struct GeProcessor::Vtype {
    std::uint32_t raw{};
    std::uint32_t tc{}, col{}, nrm{}, pos{}, wfmt{}, idx{}, nweights{}, nmorph{};
    bool through{};
    std::uint32_t woff{}, toff{}, coff{}, noff{}, poff{}, size{};
    explicit Vtype(std::uint32_t v) {
        raw = v;
        tc = v & 3u;
        col = (v >> 2u) & 7u;
        nrm = (v >> 5u) & 3u;
        pos = (v >> 7u) & 3u;
        wfmt = (v >> 9u) & 3u;
        idx = (v >> 11u) & 3u;
        nweights = wfmt ? ((v >> 14u) & 7u) + 1u : 0u;
        nmorph = (v >> 18u) & 7u;
        through = (v & (1u << 23u)) != 0u;
        const auto fmt_size = [](std::uint32_t f) { return f == 3u ? 4u : f; };
        const auto col_size = [](std::uint32_t c) { return c == 7u ? 4u : (c >= 4u ? 2u : 0u); };
        std::uint32_t off = 0, max_align = 1;
        const auto place = [&](std::uint32_t unit, std::uint32_t total) {
            if (unit == 0u || total == 0u) return off;
            off = (off + unit - 1u) & ~(unit - 1u);
            max_align = std::max(max_align, unit);
            const std::uint32_t at = off;
            off += total;
            return at;
        };
        woff = place(fmt_size(wfmt), fmt_size(wfmt) * nweights);
        toff = place(fmt_size(tc), fmt_size(tc) * 2u);
        coff = place(col_size(col), col_size(col));
        noff = place(fmt_size(nrm), fmt_size(nrm) * 3u);
        poff = place(fmt_size(pos), fmt_size(pos) * 3u);
        size = (off + max_align - 1u) & ~(max_align - 1u);
        size *= (nmorph + 1u);
    }
};

GeProcessor::GeProcessor(psprecomp::GuestMemory &memory, GpuBackend &backend)
    : memory_(memory), backend_(backend) {
    world_ = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    view_ = world_;
    tex_matrix_ = world_;
    proj_ = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    reg_[0xD5] = (271u << 10u) | 479u;
    reg_[0x48] = 0x3F8000u >> 0u;   // texture scale u/v default 1.0 (data = bits >> 8)
    reg_[0x49] = 0x3F8000u;
}

float GeProcessor::reg_float(std::uint32_t command) const { return float_from_data(reg_[command]); }

std::uint32_t GeProcessor::relative_address(std::uint32_t data) const {
    return ((((reg_[0x10] & 0x00FF0000u) << 8u) | (data & 0x00FFFFFFu)) + offset_) & 0x0FFFFFFFu;
}

std::int32_t GeProcessor::enqueue(std::uint32_t address, std::uint32_t stall, std::int32_t callback_id, bool head) {
    List list;
    list.id = next_list_id_++;
    list.pc = address & 0x0FFFFFFFu;
    list.stall = stall & 0x0FFFFFFFu;
    list.callback_id = callback_id;
    ++stats_.lists;
    if (head) queue_.push_front(std::move(list)); else queue_.push_back(std::move(list));
    const std::int32_t id = head ? queue_.front().id : queue_.back().id;
    run_queue();
    return id;
}

void GeProcessor::update_stall(std::int32_t list_id, std::uint32_t stall) {
    for (List &list : queue_) {
        if (list.id == list_id) {
            list.stall = stall & 0x0FFFFFFFu;
            break;
        }
    }
    run_queue();
}

void GeProcessor::resume() {
    for (List &list : queue_) list.paused = false;
    run_queue();
}

GeListStatus GeProcessor::status(std::int32_t list_id) const {
    for (std::size_t i = 0; i < queue_.size(); ++i) {
        const List &list = queue_[i];
        if (list.id != list_id) continue;
        if (list.paused) return GeListStatus::Paused;
        if (i != 0u) return GeListStatus::Queued;
        return list.stalled ? GeListStatus::Stalling : GeListStatus::Drawing;
    }
    return GeListStatus::Completed;
}

std::vector<GeInterrupt> GeProcessor::take_interrupts() {
    std::vector<GeInterrupt> out;
    out.swap(interrupts_);
    return out;
}

void GeProcessor::run_queue() {
    const prof::Scope prof_scope(prof::kGeQueue);
    while (!queue_.empty()) {
        List &list = queue_.front();
        if (list.paused) break;
        list.stalled = false;
        run_list(list);
        if (!list.done) break;
        queue_.pop_front();
    }
    backend_.sync();
}

void GeProcessor::run_list(List &list) {
    constexpr std::uint32_t kMaxSteps = 1u << 22u;
    std::uint32_t consecutive_unknown = 0;
    for (std::uint32_t step = 0; step < kMaxSteps; ++step) {
        if (list.stall != 0u && canon(list.pc) == canon(list.stall)) {
            list.stalled = true;
            return;
        }
        if (!memory_.contains(list.pc, 4u)) {

            log_line(category::kError, "GE list " + std::to_string(list.id) + " ran out of memory at pc");
            list.done = true;
            return;
        }
        const std::uint32_t word = memory_.load32(list.pc);
        RR_CRUMB(0, list.pc); RR_CRUMB(1, word);
        list.pc += 4u;
        const std::uint32_t command = word >> 24u;
        const std::uint32_t data = word & 0x00FFFFFFu;
        ++stats_.commands;
        if (trace_budget_ != 0u) {
            char item[40];
            std::snprintf(item, sizeof(item), " %08X:%02X%06X", list.pc - 4u, command, data);
            trace_line_ += item;
            if (--trace_budget_ == 0u || trace_line_.size() > 1500u) {
                log_line(category::kGe, "GECMD" + trace_line_);
                trace_line_.clear();
            }
        }
        if (command > 0xEAu) {
            if (++consecutive_unknown > 32u) { list.done = true; return; }
        } else {
            consecutive_unknown = 0;
        }
        reg_[command] = data;
        switch (command) {
        case 0x01u: vertex_addr_ = relative_address(data); break;
        case 0x02u: index_addr_ = relative_address(data); break;
        case 0x04u: execute_prim(data); break;
        case 0x05u: case 0x06u: ++stats_.unsupported_bezier; break;
        case 0x08u: { // JUMP
            const std::uint32_t target = relative_address(data & 0x00FFFFFCu);
            if (target == list.pc - 4u) { list.done = true; return; }
            list.pc = target;
            break;
        }
        case 0x0Au: // CALL
            if (list.stack.size() >= 32u) { list.done = true; return; }
            list.stack.push_back(Frame{list.pc, offset_, reg_[0x10]});
            list.pc = relative_address(data & 0x00FFFFFCu);
            call_site_ = list.stack.back().pc - 4u; call_target_ = list.pc;
            break;
        case 0x0Bu: // RET
            if (list.stack.empty()) { list.done = true; return; }
            list.pc = list.stack.back().pc;
            offset_ = list.stack.back().offset;
            reg_[0x10] = list.stack.back().base;
            list.stack.pop_back();
            break;
        case 0x0Cu: { // END
            const std::uint32_t preceding = memory_.contains(list.pc - 8u, 4u) ? memory_.load32(list.pc - 8u) : 0u;
            const std::uint32_t kind = preceding >> 24u;
            if (kind == 0x0Fu) {
                interrupts_.push_back(GeInterrupt{preceding & 0xFFFFu, true, list.callback_id});
                ++stats_.finishes;
                list.done = true;
                return;
            }
            if (kind != 0x0Eu) { list.done = true; return; }
            const std::uint32_t behavior = (preceding >> 16u) & 0xFFu;
            const std::uint32_t token = preceding & 0xFFFFu;
            const std::uint32_t combined = ((token << 16u) | (data & 0xFFFFu)) & 0x0FFFFFFCu;
            ++stats_.signals;
            if (behavior == 1u || behavior == 2u) {
                interrupts_.push_back(GeInterrupt{token, false, list.callback_id});
            } else if (behavior == 3u) {
                interrupts_.push_back(GeInterrupt{token, false, list.callback_id});
                list.paused = true;
                return;
            } else if (behavior >= 0x10u && behavior <= 0x16u) {
                if (behavior == 0x12u) {
                    if (list.stack.empty()) { list.done = true; return; }
                    list.pc = list.stack.back().pc;
                    offset_ = list.stack.back().offset;
                    reg_[0x10] = list.stack.back().base;
                    list.stack.pop_back();
                } else {
                    std::uint32_t target = combined;
                    if (behavior == 0x13u || behavior == 0x14u) target = (combined + list.pc - 8u) & 0x0FFFFFFFu;
                    if (behavior == 0x15u || behavior == 0x16u) target = relative_address(combined);
                    if (behavior == 0x11u || behavior == 0x14u || behavior == 0x16u) {
                        if (list.stack.size() >= 32u) { list.done = true; return; }
                        list.stack.push_back(Frame{list.pc, offset_, reg_[0x10]});
                    }
                    list.pc = target;
                }
            }
            break;
        }
        case 0x13u: offset_ = data << 8u; break;
        case 0x14u: offset_ = (list.pc - 4u) & 0x0FFFFFFFu; break;
        case 0x2Au: case 0x2Bu: case 0x3Au: case 0x3Bu: case 0x3Cu: case 0x3Du:
        case 0x3Eu: case 0x3Fu: case 0x40u: case 0x41u:
            load_matrix_data(command, data);
            break;
        case 0xC4u: load_clut(data); break;
        case 0xEAu: block_transfer(); break;
        case 0xD3u:
            if (data & 1u) ++stats_.clears;
            break;
        default: break;
        }
    }
    log_line(category::kError, "GE list " + std::to_string(list.id) + " exceeded step limit");
    list.done = true;
}

void GeProcessor::load_matrix_data(std::uint32_t command, std::uint32_t data) {
    const float value = float_from_data(data);
    switch (command) {
    case 0x2Au: bone_cursor_ = data & 0x7Fu; break;
    case 0x2Bu: if (bone_cursor_ < bones_.size()) bones_[bone_cursor_++] = value; break;
    case 0x3Au: world_cursor_ = data & 0xFu; break;
    case 0x3Bu: if (world_cursor_ < world_.size()) world_[world_cursor_++] = value; break;
    case 0x3Cu: view_cursor_ = data & 0xFu; break;
    case 0x3Du: if (view_cursor_ < view_.size()) view_[view_cursor_++] = value; break;
    case 0x3Eu: proj_cursor_ = data & 0xFu; break;
    case 0x3Fu: if (proj_cursor_ < proj_.size()) proj_[proj_cursor_++] = value; break;
    case 0x40u: tex_cursor_ = data & 0xFu; break;
    case 0x41u: if (tex_cursor_ < tex_matrix_.size()) tex_matrix_[tex_cursor_++] = value; break;
    default: break;
    }
}

void GeProcessor::load_clut(std::uint32_t data) {
    const std::uint32_t address = (reg_[0xB0] & 0x00FFFFFFu) | ((reg_[0xB1] & 0x000F0000u) << 8u);
    const std::uint32_t bytes = std::min<std::uint32_t>((data & 0x3Fu) * 32u, 1024u);
    const std::uint32_t source = gpu_address(address);
    if (bytes != 0u && memory_.contains(source, bytes)) {
        // Palettes the GPU renders (the sun glare's brightness rows, 0x04195140 on Seaside, 0x041E4940 on the city course)
        // may be a frame old; a synchronous read every frame costs ~20 fps at 144.
        if (source >= 0x04000000u && source < 0x04200000u) backend_.vram_read_late_ok(source, bytes);
        else backend_.vram_read(source, bytes);
        memory_.copy_out(source, std::span<std::uint8_t>(clut_.data(), bytes));
        // Diagnostic: log each distinct palette (by content hash) with its average colour.
        std::uint64_t hash = 1469598103934665603ull;
        for (std::uint32_t i = 0; i < bytes; ++i) hash = (hash ^ clut_[i]) * 1099511628211ull;
        clut_src_ = source; clut_hash_ = hash;
        char tag[32];
        std::snprintf(tag, sizeof tag, "CLUT%016llX", static_cast<unsigned long long>(hash));
        static std::uint32_t clut_logged = 0u, purple_logged = 0u;
        if (seen_states_.find(tag) == seen_states_.end() && clut_logged < 1200u) {
            seen_states_.emplace(tag, 1u);
            ++clut_logged;
            const std::uint32_t fmt = reg_[0xC5] & 3u;
            const std::uint32_t entries = fmt == 3u ? bytes / 4u : bytes / 2u;
            std::uint64_t sr = 0, sg = 0, sb = 0;
            std::uint32_t first[4]{};
            for (std::uint32_t i = 0; i < entries; ++i) {
                std::uint32_t c;
                if (fmt == 3u) { std::memcpy(&c, clut_.data() + i * 4u, 4); }
                else {
                    std::uint16_t v; std::memcpy(&v, clut_.data() + i * 2u, 2);
                    if (fmt == 0u) { const std::uint32_t r = v & 31u, g = (v >> 5u) & 63u, b = (v >> 11u) & 31u;
                        c = ((r << 3u) | (r >> 2u)) | (((g << 2u) | (g >> 4u)) << 8u) | (((b << 3u) | (b >> 2u)) << 16u); }
                    else if (fmt == 1u) { const std::uint32_t r = v & 31u, g = (v >> 5u) & 31u, b = (v >> 10u) & 31u;
                        c = ((r << 3u) | (r >> 2u)) | (((g << 3u) | (g >> 2u)) << 8u) | (((b << 3u) | (b >> 2u)) << 16u); }
                    else { c = ((v & 15u) * 17u) | ((((v >> 4u) & 15u) * 17u) << 8u) | ((((v >> 8u) & 15u) * 17u) << 16u); }
                }
                sr += c & 255u; sg += (c >> 8u) & 255u; sb += (c >> 16u) & 255u;
                if (i < 4u) first[i] = c;
            }
            const std::uint64_t n = entries != 0u ? entries : 1u;
            char line[320];
            std::snprintf(line, sizeof line,
                "GECLUT frame=%llu addr=%08X fmt=%u entries=%u avgRGB=%llu,%llu,%llu first=%08X,%08X,%08X,%08X",
                static_cast<unsigned long long>(frame_), source, fmt, entries,
                static_cast<unsigned long long>(sr / n), static_cast<unsigned long long>(sg / n),
                static_cast<unsigned long long>(sb / n), first[0], first[1], first[2], first[3]);
            log_line(category::kGe, line);
            if (entries <= 16u && fmt == 3u) {
                std::string all = "GECLUTALL frame=" + std::to_string(frame_) + " :";
                for (std::uint32_t i = 0; i < entries; ++i) {
                    std::uint32_t c; std::memcpy(&c, clut_.data() + i * 4u, 4);
                    char b[16]; std::snprintf(b, sizeof b, " %08X", c); all += b;
                }
                log_line(category::kGe, all);
            }
            // Purple entries: red and blue both well above green.
            std::uint32_t purple = 0u;
            std::string samples;
            for (std::uint32_t i = 0; i < entries; ++i) {
                std::uint32_t c;
                if (fmt == 3u) std::memcpy(&c, clut_.data() + i * 4u, 4);
                else { std::uint16_t v; std::memcpy(&v, clut_.data() + i * 2u, 2); c = v; }
                if (fmt != 3u) break;
                const std::uint32_t r = c & 255u, g = (c >> 8u) & 255u, b = (c >> 16u) & 255u;
                if (r > g + 24u && b > g + 24u) {
                    ++purple;
                    if (samples.size() < 200u) {
                        char sbuf[24];
                        std::snprintf(sbuf, sizeof sbuf, " [%u]=%08X", i, c);
                        samples += sbuf;
                    }
                }
            }
            if (purple != 0u && purple_logged < 300u) {
                ++purple_logged;
                log_line(category::kGe, "GEPURPLECLUT frame=" + std::to_string(frame_) + " addr=" +
                         std::to_string(source) + " purple_entries=" + std::to_string(purple) + "/" +
                         std::to_string(entries) + samples);
            }
        }
    }
}

void GeProcessor::block_transfer() {
    const std::uint32_t src = gpu_address((reg_[0xB2] & 0x00FFFFFFu) | ((reg_[0xB3] & 0x00FF0000u) << 8u));
    const std::uint32_t dst = gpu_address((reg_[0xB4] & 0x00FFFFFFu) | ((reg_[0xB5] & 0x00FF0000u) << 8u));
    const std::uint32_t src_stride = reg_[0xB3] & 0x7FFu, dst_stride = reg_[0xB5] & 0x7FFu;
    const std::uint32_t sx = reg_[0xEB] & 0x3FFu, sy = (reg_[0xEB] >> 10u) & 0x3FFu;
    const std::uint32_t dx = reg_[0xEC] & 0x3FFu, dy = (reg_[0xEC] >> 10u) & 0x3FFu;
    const std::uint32_t w = (reg_[0xEE] & 0x3FFu) + 1u, h = ((reg_[0xEE] >> 10u) & 0x3FFu) + 1u;
    const std::uint32_t bpp = (reg_[0xEA] & 1u) ? 4u : 2u;
    std::uint32_t skipped = 0u;
    // Keep GPU-resident render targets coherent with memory around the copy.
    backend_.set_read_reason("xfer");
    backend_.vram_read(src + (sy * src_stride + sx) * bpp, ((h - 1u) * src_stride + w) * bpp);
    backend_.vram_read(dst + (dy * dst_stride + dx) * bpp, ((h - 1u) * dst_stride + w) * bpp);
    backend_.set_read_reason("ge");
    if (std::getenv("RR_XFER_LOG") != nullptr) {   // every block transfer (limited): source, destination, size
        static int n = 0;
        if (n < 3000) { ++n; char b[160]; std::snprintf(b, sizeof b, "XFER f=%llu src=%08X+%u,%u stride=%u dst=%08X+%u,%u stride=%u %ux%u bpp=%u", static_cast<unsigned long long>(frame_), src, sx, sy, src_stride, dst, dx, dy, dst_stride, w, h, bpp); log_line(category::kGe, b); }
    }
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::uint32_t s = src + ((sy + row) * src_stride + sx) * bpp;
        const std::uint32_t d = dst + ((dy + row) * dst_stride + dx) * bpp;
        const std::uint8_t *sp = static_cast<const psprecomp::GuestMemory &>(memory_).raw_pointer(s, w * bpp);
        std::uint8_t *dp = memory_.raw_pointer(d, w * bpp);
        if (sp != nullptr && dp != nullptr) std::memmove(dp, sp, w * bpp);
        else ++skipped;
    }
    backend_.vram_write(dst + (dy * dst_stride + dx) * bpp, ((h - 1u) * dst_stride + w) * bpp);
    xfers_.push_back({frame_, src, dst, dst + ((dy + h) * dst_stride + dx + w) * bpp, w, h, bpp, skipped});
    if (xfers_.size() > 600u) xfers_.pop_front();
}

std::string GeProcessor::provenance(std::uint32_t address, std::uint32_t length) const {
    std::string out;
    const std::uint32_t end = address + length;
    int found = 0;
    for (auto it = xfers_.rbegin(); it != xfers_.rend() && found < 2; ++it) {
        const std::uint32_t begin = it->dst;
        if (address < it->dst_end && end > begin) {
            char b[160];
            std::snprintf(b, sizeof b, " xfer[f%llu src=%08X dst=%08X %ux%u bpp=%u skipped=%u]",
                          static_cast<unsigned long long>(it->frame), it->src, it->dst, it->w, it->h, it->bpp, it->skipped);
            out += b; ++found;
        }
    }
    if (found == 0) out += " xfer[none]";
    for (const TargetRec &t : targets_) {
        const std::uint32_t bytes = t.stride * 272u * (t.format == 3u ? 4u : 2u);
        if (address >= t.fb_addr && address < t.fb_addr + bytes) {
            char b[96];
            std::snprintf(b, sizeof b, " drawtarget[%08X fmt=%u stride=%u lastdraw=f%llu]", t.fb_addr, t.format, t.stride,
                          static_cast<unsigned long long>(t.frame));
            out += b;
        }
    }
    return out;
}

GpuDraw GeProcessor::build_draw() const {
    GpuDraw d;
    d.fb_addr = gpu_address((reg_[0x9C] & 0x00FFFFFFu) | ((reg_[0x9D] & 0x00FF0000u) << 8u));
    d.fb_stride = std::max<std::uint32_t>(reg_[0x9D] & 0xFFFFu, 1u);
    d.fb_format = reg_[0xD2] & 3u;
    d.z_addr = gpu_address((reg_[0x9E] & 0x00FFFFFFu) | ((reg_[0x9F] & 0x00FF0000u) << 8u));
    d.z_stride = std::max<std::uint32_t>(reg_[0x9F] & 0xFFFFu, 1u);
    static const bool no_stencil = std::getenv("RR_NO_STENCIL") != nullptr;
    d.stencil_test = (reg_[0x24] & 1u) != 0u && !no_stencil;
    d.stencil_func = reg_[0xDC] & 7u; d.stencil_ref = (reg_[0xDC] >> 8u) & 0xFFu; d.stencil_mask = (reg_[0xDC] >> 16u) & 0xFFu;
    d.stencil_sfail = reg_[0xDD] & 7u; d.stencil_zfail = (reg_[0xDD] >> 8u) & 7u; d.stencil_zpass = (reg_[0xDD] >> 16u) & 7u;
    static const bool no_fog = std::getenv("RR_NO_FOG") != nullptr;
    d.fog = (reg_[0x1F] & 1u) != 0u && !no_fog;
    d.fog_color = reg_[0xCF] & 0x00FFFFFFu;
    d.clear_mode = (reg_[0xD3] & 1u) != 0u;
    d.clear_color = (reg_[0xD3] & 0x100u) != 0u;
    d.clear_alpha = (reg_[0xD3] & 0x200u) != 0u;
    d.clear_depth = (reg_[0xD3] & 0x400u) != 0u;
    d.tex_enable = (reg_[0x1E] & 1u) != 0u;
    d.tex_addr = gpu_address((reg_[0xA0] & 0x00FFFFFFu) | ((reg_[0xA8] & 0x00FF0000u) << 8u));
    d.tex_stride = reg_[0xA8] & 0xFFFFu;
    d.tex_width = 1u << std::min<std::uint32_t>(reg_[0xB8] & 0xFu, 9u);
    d.tex_height = 1u << std::min<std::uint32_t>((reg_[0xB8] >> 8u) & 0xFu, 9u);
    d.tex_format = reg_[0xC3] & 0xFu;
    d.tex_swizzled = (reg_[0xC2] & 1u) != 0u;
    d.tex_linear = ((reg_[0xC6] >> 8u) & 1u) != 0u;
    d.clamp_u = (reg_[0xC7] & 1u) != 0u;
    d.clamp_v = ((reg_[0xC7] >> 8u) & 1u) != 0u;
    d.tex_func = reg_[0xC9] & 7u;
    d.tex_alpha = ((reg_[0xC9] >> 8u) & 1u) != 0u;
    d.tex_double = ((reg_[0xC9] >> 16u) & 1u) != 0u;
    d.tex_env = reg_[0xCA] & 0x00FFFFFFu;
    d.clut_format = reg_[0xC5] & 3u;
    d.clut_shift = (reg_[0xC5] >> 2u) & 0x1Fu;
    d.clut_mask = (reg_[0xC5] >> 8u) & 0xFFu;
    d.clut_start = (reg_[0xC5] >> 16u) & 0x1Fu;
    d.clut = clut_.data();
    d.blend = (reg_[0x21] & 1u) != 0u;
    d.blend_src = reg_[0xDF] & 0xFu;
    d.blend_dst = (reg_[0xDF] >> 4u) & 0xFu;
    d.blend_eq = (reg_[0xDF] >> 8u) & 7u;
    d.fix_a = reg_[0xE0] & 0x00FFFFFFu;
    d.fix_b = reg_[0xE1] & 0x00FFFFFFu;
    d.alpha_test = (reg_[0x22] & 1u) != 0u;
    d.atest_func = reg_[0xDB] & 7u;
    d.atest_ref = (reg_[0xDB] >> 8u) & 0xFFu;
    d.atest_mask = (reg_[0xDB] >> 16u) & 0xFFu;
    d.color_test = (reg_[0x26] & 1u) != 0u;
    d.ctest_func = reg_[0xD8] & 3u;
    d.ctest_ref = reg_[0xD9] & 0xFFFFFFu;
    d.ctest_mask = reg_[0xDA] & 0xFFFFFFu;
    d.depth_test = (reg_[0x23] & 1u) != 0u;
    d.depth_func = reg_[0xDE] & 7u;
    d.depth_write = (reg_[0xE7] & 1u) == 0u;
    d.mask_rgb = reg_[0xE8] & 0x00FFFFFFu;
    d.mask_alpha = reg_[0xE9] & 0xFFu;
    d.scissor_x0 = static_cast<std::int32_t>(reg_[0xD4] & 0x3FFu);
    d.scissor_y0 = static_cast<std::int32_t>((reg_[0xD4] >> 10u) & 0x3FFu);
    d.scissor_x1 = static_cast<std::int32_t>(reg_[0xD5] & 0x3FFu);
    d.scissor_y1 = static_cast<std::int32_t>((reg_[0xD5] >> 10u) & 0x3FFu);
    // The 32 columns right of the 480 visible ones in a 16-bit stride-512 screen buffer are not part of the picture; this game keeps
    // palettes there.  A full-width effect (the sun glare draws in 64-column bands up to x=511) must not blend into them.
    static const bool no_margin_clip = std::getenv("RR_NO_MARGIN_CLIP") != nullptr;   // A/B switch
    if (!no_margin_clip && (!d.clear_mode || d.clear_color || d.clear_alpha) && d.fb_format != 3u && d.fb_stride == 512u && d.scissor_x1 > 479) d.scissor_x1 = 479;
    return d;
}

namespace {

struct RawVertex {
    float pos[3]{};
    float uv[2]{};
    float nrm[3]{};
    float weights[8]{};
    std::uint32_t color{0xFFFFFFFFu};  // 0xAABBGGRR
    bool has_color{};
};

std::uint32_t expand_565(std::uint32_t p) {
    const std::uint32_t r = p & 31u, g = (p >> 5u) & 63u, b = (p >> 11u) & 31u;
    return ((r << 3u) | (r >> 2u)) | (((g << 2u) | (g >> 4u)) << 8u) | (((b << 3u) | (b >> 2u)) << 16u) | 0xFF000000u;
}
std::uint32_t expand_5551(std::uint32_t p) {
    const std::uint32_t r = p & 31u, g = (p >> 5u) & 31u, b = (p >> 10u) & 31u;
    return ((r << 3u) | (r >> 2u)) | (((g << 3u) | (g >> 2u)) << 8u) | (((b << 3u) | (b >> 2u)) << 16u) |
           ((p & 0x8000u) ? 0xFF000000u : 0u);
}
std::uint32_t expand_4444(std::uint32_t p) {
    return ((p & 15u) * 17u) | ((((p >> 4u) & 15u) * 17u) << 8u) | ((((p >> 8u) & 15u) * 17u) << 16u) |
           ((((p >> 12u) & 15u) * 17u) << 24u);
}

} // namespace

void GeProcessor::execute_prim(std::uint32_t data) {
    const prof::Scope prof_scope(prof::kGePrim);
    const std::uint32_t prim = (data >> 16u) & 7u;
    const std::uint32_t count = data & 0xFFFFu;
    const Vtype vt(reg_[0x12]);
    ++stats_.prims;
    stats_.vertices += count;
    if (count == 0u || prim > 6u) return;
    if (skip_prims_) {
        const std::uint32_t unit = vt.idx == 1u ? 1u : (vt.idx == 2u ? 2u : (vt.idx == 3u ? 4u : 0u));
        if (unit == 0u) vertex_addr_ += count * vt.size; else index_addr_ += count * unit;
        return;
    }
    if (vt.through) ++stats_.through;

    const bool lighting = (reg_[0x17] & 1u) != 0u && !vt.through;
    static const bool no_lighting = std::getenv("RR_NO_LIGHTING") != nullptr;
    if (lighting) ++stats_.lit_prims;
    if (vt.nweights != 0u) ++stats_.skinned;

    std::optional<prof::Scope> sub_scope;
    sub_scope.emplace(prof::kPrimSetup);
    GpuDraw draw = build_draw();
    draw.through = vt.through;
    if (draw.tex_enable && (draw.tex_format == 4u || draw.tex_format == 5u)) glyph_patch_draw(memory_, draw);   // controller button pictures
    // The two diagnostics below only matter the first time a state shows up.  Formatting their text for every draw
    // cost several microseconds each (thousands of draws per frame), so a cheap hash of the inputs decides first.
    const auto fnv = [](std::uint64_t h, std::uint32_t v) { return (h ^ v) * 1099511628211ull; };
    static std::unordered_set<std::uint64_t> seen_light_hash, seen_state_hash;
    bool light_new = false;
    if (lighting || draw.tex_enable) {
        std::uint64_t h = lighting ? 0x9E3779B97F4A7C15ull : 0x1234567ull;
        for (std::uint32_t r = 0x53u; r <= 0x5Eu; ++r) h = fnv(h, reg_[r] & 0xFFFFFFu);
        for (std::uint32_t r = 0x8Fu; r <= 0x9Au; ++r) h = fnv(h, reg_[r] & 0xFFFFFFu);
        light_new = seen_light_hash.insert(h).second;
    }
    if (light_new) {
        // Diagnostic: log each distinct lighting/material setup (colours that the renderer does not model).
        char lk[400];
        int off = std::snprintf(lk, sizeof lk, "LIGHTSET");
        for (std::uint32_t r = 0x53u; r <= 0x5Eu; ++r)
            off += std::snprintf(lk + off, sizeof lk - static_cast<std::size_t>(off), " %02X=%06X", r, reg_[r] & 0xFFFFFFu);
        for (std::uint32_t r = 0x8Fu; r <= 0x9Au; ++r)
            off += std::snprintf(lk + off, sizeof lk - static_cast<std::size_t>(off), " %02X=%06X", r, reg_[r] & 0xFFFFFFu);
        auto lit = seen_states_.find(lk);
        if (lit == seen_states_.end()) {
            static std::uint32_t light_logged = 0u;
            if (light_logged < 150u) {
                ++light_logged;
                seen_states_.emplace(lk, 1u);
                log_line(category::kGe, "GELIGHT frame=" + std::to_string(frame_) + (lighting ? " lit " : " unlit ") + lk);
            }
        }
    }
    bool state_new = false;
    {
        std::uint64_t h = 0xCBF29CE484222325ull;
        for (std::uint32_t v : {vt.through ? 1u : 0u, draw.tex_enable ? 1u : 0u, draw.tex_format, draw.clut_format, draw.tex_func,
                                draw.tex_alpha ? 1u : 0u, draw.tex_double ? 1u : 0u, draw.tex_env, reg_[0xC0] & 3u,
                                draw.blend ? 1u : 0u, draw.blend_src, draw.blend_dst, draw.blend_eq, draw.fix_a, draw.fix_b,
                                draw.alpha_test ? 1u : 0u, draw.depth_test ? 1u : 0u, draw.mask_rgb, draw.mask_alpha,
                                reg_[0x1F] & 1u, reg_[0xCF] & 0xFFFFFFu, reg_[0x17] & 1u, reg_[0x24] & 1u, reg_[0x27] & 1u,
                                reg_[0x28] & 1u, draw.fb_format, reg_[0xDC] & 0xFFFFFFu, reg_[0xDD] & 0xFFFFFFu, reg_[0xD8] & 0xFFFFFFu,
                                reg_[0xD9] & 0xFFFFFFu, reg_[0xDA] & 0xFFFFFFu, reg_[0xCD], reg_[0xCE]})
            h = fnv(h, v);
        state_new = seen_state_hash.size() < 20000u && seen_state_hash.insert(h).second;
    }
    if (state_new) {
        // Diagnostic: log each distinct draw state the first time it is used.
        char key[512];
        std::snprintf(key, sizeof key,
            "through=%d tex=%d fmt=%u clutfmt=%u tfunc=%u talpha=%d tdouble=%d texenv=%06X texmap=%u "
            "blend=%d bsrc=%u bdst=%u beq=%u fixa=%06X fixb=%06X atest=%d depth=%d "
            "maskrgb=%06X maskalpha=%02X fog=%d fogcol=%06X light=%d stencil=%d colortest=%d logicop=%d fbfmt=%u sfunc=%06X sop=%06X ctfunc=%06X ctref=%06X ctmask=%06X fogend=%g fogslope=%g",
            vt.through ? 1 : 0, draw.tex_enable ? 1 : 0, draw.tex_format, draw.clut_format, draw.tex_func,
            draw.tex_alpha ? 1 : 0, draw.tex_double ? 1 : 0, draw.tex_env, reg_[0xC0] & 3u,
            draw.blend ? 1 : 0, draw.blend_src, draw.blend_dst, draw.blend_eq, draw.fix_a, draw.fix_b,
            draw.alpha_test ? 1 : 0, draw.depth_test ? 1 : 0, draw.mask_rgb, draw.mask_alpha,
            static_cast<int>(reg_[0x1F] & 1u), reg_[0xCF] & 0xFFFFFFu, static_cast<int>(reg_[0x17] & 1u),
            static_cast<int>(reg_[0x24] & 1u), static_cast<int>(reg_[0x27] & 1u), static_cast<int>(reg_[0x28] & 1u),
            draw.fb_format, reg_[0xDC] & 0xFFFFFFu, reg_[0xDD] & 0xFFFFFFu, reg_[0xD8] & 0xFFFFFFu, reg_[0xD9] & 0xFFFFFFu, reg_[0xDA] & 0xFFFFFFu,
            static_cast<double>(reg_float(0xCD)), static_cast<double>(reg_float(0xCE)));
        auto it = seen_states_.find(key);
        if (it == seen_states_.end()) {
            if (seen_states_.size() < 5000u) {
                seen_states_.emplace(key, 1u);
                log_line(category::kGe, "GESTATE frame=" + std::to_string(frame_) + " " + key);
            }
        } else {
            ++it->second;
        }
    }
    if (draw.tex_enable) ++stats_.textured;
    sub_scope.reset();

    const std::uint32_t tex_map_mode = reg_[0xC0] & 3u;
    {   // RR_TMAP_LOG=frame[,count]: every draw of those frames with its texture-mapping mode (reflections are mode 1/2)
        static const std::pair<std::uint64_t, std::uint64_t> tl = [] {
            const char *e = std::getenv("RR_TMAP_LOG");
            if (!e) return std::pair<std::uint64_t, std::uint64_t>{~0ull, 0ull};
            std::uint64_t a = std::strtoull(e, nullptr, 0), n = 1;
            if (const char *c = std::strchr(e, ',')) n = std::strtoull(c + 1, nullptr, 0);
            return std::pair<std::uint64_t, std::uint64_t>{a, n};
        }();
        if (frame_ >= tl.first && frame_ < tl.first + tl.second) {
            char b[400];
            std::snprintf(b, sizeof b, "TMAP f=%llu call=%08X->%08X v=%08X i=%08X mode=%u src=%u envl=%u,%u n=%u through=%d tex=%d@%08X %ux%u fmt=%u clut=%u tfunc=%u lin=%d fb=%08X blend=%d s%u d%u fa%06X fb%06X light=%d world_t=%.1f,%.1f,%.1f",
                          static_cast<unsigned long long>(frame_), call_site_, call_target_, vertex_addr_, index_addr_, tex_map_mode, (reg_[0xC0] >> 8u) & 3u, reg_[0xC1] & 3u, (reg_[0xC1] >> 8u) & 3u, count,
                          vt.through ? 1 : 0, draw.tex_enable ? 1 : 0, draw.tex_addr, draw.tex_width, draw.tex_height, draw.tex_format, draw.clut_format,
                          draw.tex_func, draw.tex_linear ? 1 : 0, draw.fb_addr, draw.blend ? 1 : 0, draw.blend_src, draw.blend_dst, draw.fix_a, draw.fix_b,
                          static_cast<int>(reg_[0x17] & 1u), static_cast<double>(world_[9]), static_cast<double>(world_[10]), static_cast<double>(world_[11]));
            log_line(category::kGe, b);
        }
    }

    const std::uint32_t ambient_color = (reg_[0x55] & 0x00FFFFFFu) | ((reg_[0x58] & 0xFFu) << 24u);
    const float scale_u = reg_float(0x48), scale_v = reg_float(0x49);
    const float off_u = reg_float(0x4A), off_v = reg_float(0x4B);
    const float vp_xs = reg_float(0x42), vp_ys = reg_float(0x43), vp_zs = reg_float(0x44);
    const float vp_xc = reg_float(0x45), vp_yc = reg_float(0x46), vp_zc = reg_float(0x47);
    const float scr_off_x = static_cast<float>(reg_[0x4C] & 0xFFFFu) / 16.0f;
    const float scr_off_y = static_cast<float>(reg_[0x4D] & 0xFFFFu) / 16.0f;
    const float tex_w = static_cast<float>(draw.tex_width), tex_h = static_cast<float>(draw.tex_height);

    const bool fog_on = draw.fog && !vt.through;
    const float fog_end = reg_float(0xCD), fog_slope = reg_float(0xCE);
    struct Clip { float x, y, z, w; GpuVertex v; };
    static thread_local std::vector<Clip> clip;   // reused between draws: no allocation per primitive
    clip.clear();
    clip.reserve(count);

    const std::uint32_t vbase = vertex_addr_;
    const std::uint32_t index_unit = vt.idx == 1u ? 1u : (vt.idx == 2u ? 2u : (vt.idx == 3u ? 4u : 0u));
    draw.src_addr = index_unit != 0u ? index_addr_ : vbase;

    sub_scope.emplace(prof::kPrimVerts);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t index = i;
        if (index_unit != 0u) {
            const std::uint32_t ia = index_addr_ + i * index_unit;
            const std::uint8_t *ip = memory_.raw_pointer(ia, index_unit);
            if (ip != nullptr) { index = 0u; std::memcpy(&index, ip, index_unit); }
            else {
                if (!memory_.contains(ia, index_unit)) return;
                index = index_unit == 1u ? memory_.load8(ia) : (index_unit == 2u ? memory_.load16(ia) : memory_.load32(ia));
            }
        }
        const std::uint32_t va = vbase + index * vt.size;
        // One address translation per vertex instead of one per byte read (GuestMemory::loadN resolves every byte).
        std::uint8_t vbuf[256];
        const std::uint8_t *vp = memory_.raw_pointer(va, std::max(vt.size, 1u));
        if (vp == nullptr) {
            if (!memory_.contains(va, std::max(vt.size, 1u)) || vt.size > sizeof vbuf) return;
            for (std::uint32_t q = 0; q < vt.size; ++q) vbuf[q] = memory_.load8(va + q);
            vp = vbuf;
        }
        const auto rd8 = [vp](std::uint32_t o) -> std::uint32_t { return vp[o]; };
        const auto rd16 = [vp](std::uint32_t o) -> std::uint32_t { std::uint16_t v; std::memcpy(&v, vp + o, 2); return v; };
        const auto rd32 = [vp](std::uint32_t o) -> std::uint32_t { std::uint32_t v; std::memcpy(&v, vp + o, 4); return v; };

        RawVertex rv;
        // weights
        if (vt.nweights) {
            for (std::uint32_t w = 0; w < vt.nweights && w < 8u; ++w) {
                if (vt.wfmt == 1u) rv.weights[w] = rd8(vt.woff + w) / 128.0f;
                else if (vt.wfmt == 2u) rv.weights[w] = rd16(vt.woff + w * 2u) / 32768.0f;
                else { const std::uint32_t b = rd32(vt.woff + w * 4u); std::memcpy(&rv.weights[w], &b, 4); }
            }
        }
        // texcoords
        if (vt.tc) {
            for (std::uint32_t k = 0; k < 2u; ++k) {
                float value = 0.0f;
                if (vt.tc == 1u) { const float raw = rd8(vt.toff + k); value = vt.through ? raw : raw / 128.0f; }
                else if (vt.tc == 2u) { const float raw = rd16(vt.toff + k * 2u); value = vt.through ? raw : raw / 32768.0f; }
                else { const std::uint32_t b = rd32(vt.toff + k * 4u); std::memcpy(&value, &b, 4); }
                rv.uv[k] = value;
            }
        }
        // normal
        if (vt.nrm) {
            for (std::uint32_t k = 0; k < 3u; ++k) {
                float value = 0.0f;
                if (vt.nrm == 1u) value = static_cast<std::int8_t>(rd8(vt.noff + k)) / 127.0f;
                else if (vt.nrm == 2u) value = static_cast<std::int16_t>(rd16(vt.noff + k * 2u)) / 32767.0f;
                else { const std::uint32_t b = rd32(vt.noff + k * 4u); std::memcpy(&value, &b, 4); }
                rv.nrm[k] = value;
            }
        }
        // color
        if (vt.col >= 4u) {
            rv.has_color = true;
            if (vt.col == 7u) rv.color = rd32(vt.coff);
            else {
                const std::uint32_t p = rd16(vt.coff);
                rv.color = vt.col == 4u ? expand_565(p) : (vt.col == 5u ? expand_5551(p) : expand_4444(p));
            }
        } else {
            rv.color = ambient_color;
        }
        // position
        for (std::uint32_t k = 0; k < 3u; ++k) {
            float value = 0.0f;
            if (vt.pos == 1u) {
                const std::int32_t raw = vt.through && k == 2u ? static_cast<std::int32_t>(rd8(vt.poff + k))
                                                               : static_cast<std::int8_t>(rd8(vt.poff + k));
                value = vt.through ? static_cast<float>(raw) : raw / 128.0f;
            } else if (vt.pos == 2u) {
                const std::int32_t raw = vt.through && k == 2u ? static_cast<std::int32_t>(rd16(vt.poff + k * 2u))
                                                               : static_cast<std::int16_t>(rd16(vt.poff + k * 2u));
                value = vt.through ? static_cast<float>(raw) : raw / 32768.0f;
            } else if (vt.pos == 3u) {
                const std::uint32_t b = rd32(vt.poff + k * 4u);
                std::memcpy(&value, &b, 4);
            }
            rv.pos[k] = value;
        }

        Clip c{};
        GpuVertex &gv = c.v;
        gv.r = static_cast<float>(rv.color & 0xFFu);
        gv.g = static_cast<float>((rv.color >> 8u) & 0xFFu);
        gv.b = static_cast<float>((rv.color >> 16u) & 0xFFu);
        gv.a = static_cast<float>(rv.color >> 24u);
        if (vt.through) {
            gv.x = rv.pos[0];
            gv.y = rv.pos[1];
            gv.z = rv.pos[2];
            gv.inv_w = 1.0f;
            gv.u = rv.uv[0];
            gv.v = rv.uv[1];
        } else {
            float p[3] = {rv.pos[0], rv.pos[1], rv.pos[2]};
            if (vt.nweights != 0u) {
                float acc[3] = {0, 0, 0};
                for (std::uint32_t w = 0; w < vt.nweights && w < 8u; ++w) {
                    const float *m = &bones_[w * 12u];
                    for (int r = 0; r < 3; ++r)
                        acc[r] += rv.weights[w] * (m[r] * p[0] + m[3 + r] * p[1] + m[6 + r] * p[2] + m[9 + r]);
                }
                p[0] = acc[0]; p[1] = acc[1]; p[2] = acc[2];
            }
            float wv[3], vv[3];
            for (int r = 0; r < 3; ++r) wv[r] = world_[r] * p[0] + world_[3 + r] * p[1] + world_[6 + r] * p[2] + world_[9 + r];
            for (int r = 0; r < 3; ++r) vv[r] = view_[r] * wv[0] + view_[3 + r] * wv[1] + view_[6 + r] * wv[2] + view_[9 + r];
            float out[4];
            for (int r = 0; r < 4; ++r) out[r] = proj_[r] * vv[0] + proj_[4 + r] * vv[1] + proj_[8 + r] * vv[2] + proj_[12 + r];
            c.x = out[0]; c.y = out[1]; c.z = out[2]; c.w = out[3];
            if (fog_on) gv.fog = std::clamp((fog_end - c.w) * fog_slope, 0.0f, 1.0f);
            if (fog_on) {
                static double w_min = 1e30, w_max = -1e30, z_min = 1e30, z_max = -1e30, f_sum = 0; static std::uint64_t n = 0, last_frame = 0;
                w_min = std::min<double>(w_min, c.w); w_max = std::max<double>(w_max, c.w);
                z_min = std::min<double>(z_min, vv[2]); z_max = std::max<double>(z_max, vv[2]); f_sum += gv.fog; ++n;
                if (frame_ >= last_frame + 150u) {
                    last_frame = frame_;
                    char b2[600];
                    std::snprintf(b2, sizeof b2, "GEFOG frame=%llu n=%llu w=[%g..%g] viewz=[%g..%g] meanfog=%.3f end=%g slope=%g proj=[%g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g] view_row_z=[%g %g %g %g]",
                        static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(n), w_min, w_max, z_min, z_max, f_sum / static_cast<double>(n),
                        static_cast<double>(fog_end), static_cast<double>(fog_slope),
                        static_cast<double>(proj_[0]), static_cast<double>(proj_[1]), static_cast<double>(proj_[2]), static_cast<double>(proj_[3]),
                        static_cast<double>(proj_[4]), static_cast<double>(proj_[5]), static_cast<double>(proj_[6]), static_cast<double>(proj_[7]),
                        static_cast<double>(proj_[8]), static_cast<double>(proj_[9]), static_cast<double>(proj_[10]), static_cast<double>(proj_[11]),
                        static_cast<double>(proj_[12]), static_cast<double>(proj_[13]), static_cast<double>(proj_[14]), static_cast<double>(proj_[15]),
                        static_cast<double>(view_[2]), static_cast<double>(view_[5]), static_cast<double>(view_[8]), static_cast<double>(view_[11]));
                    log_line(category::kGe, b2);
                    w_min = z_min = 1e30; w_max = z_max = -1e30; f_sum = 0; n = 0;
                }
            }
            if (lighting && !no_lighting) {
                // PSP fixed-function lighting: colour = emissive + (scene ambient + sum of light ambient) * material ambient
                //                                      + sum of (light diffuse * material diffuse * max(N.L, 0) + specular), all scaled by attenuation.
                const auto unpack = [](std::uint32_t c, float o[3]) {
                    o[0] = static_cast<float>(c & 255u) / 255.0f; o[1] = static_cast<float>((c >> 8u) & 255u) / 255.0f; o[2] = static_cast<float>((c >> 16u) & 255u) / 255.0f;
                };
                const std::uint32_t update = reg_[0x53] & 7u;
                const bool vc = rv.has_color;
                float mat_a[3], mat_d[3], mat_s[3], emis[3], scene_a[3];
                unpack(vc && (update & 1u) ? rv.color : reg_[0x55], mat_a);
                unpack(vc && (update & 2u) ? rv.color : reg_[0x56], mat_d);
                unpack(vc && (update & 4u) ? rv.color : reg_[0x57], mat_s);
                unpack(reg_[0x54], emis);
                unpack(reg_[0x5C], scene_a);
                const float mat_alpha = vc && (update & 1u) ? static_cast<float>(rv.color >> 24u) : static_cast<float>(reg_[0x58] & 255u);
                float n[3];
                for (int r = 0; r < 3; ++r) n[r] = world_[r] * rv.nrm[0] + world_[3 + r] * rv.nrm[1] + world_[6 + r] * rv.nrm[2];
                const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nl > 1e-8f) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
                const float spec_pow = reg_float(0x5B);
                float amb_sum[3] = {scene_a[0], scene_a[1], scene_a[2]}, diff_sum[3] = {0, 0, 0}, spec_sum[3] = {0, 0, 0};
                for (std::uint32_t li = 0; li < 4u; ++li) {
                    if ((reg_[0x18u + li] & 1u) == 0u) continue;
                    const std::uint32_t type = reg_[0x5Fu + li];
                    const std::uint32_t comp = type & 3u, kind = (type >> 8u) & 3u;
                    float lp[3] = {reg_float(0x63u + 3u * li), reg_float(0x64u + 3u * li), reg_float(0x65u + 3u * li)};
                    float l[3];
                    float att = 1.0f;
                    if (kind == 0u) {
                        for (int r = 0; r < 3; ++r) l[r] = lp[r];
                    } else {
                        for (int r = 0; r < 3; ++r) l[r] = lp[r] - wv[r];
                        const float d = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                        const float k0 = reg_float(0x7Bu + 3u * li), k1 = reg_float(0x7Cu + 3u * li), k2 = reg_float(0x7Du + 3u * li);
                        const float den = k0 + k1 * d + k2 * d * d;
                        att = den > 1e-12f ? std::min(1.0f / den, 1.0f) : 1.0f;
                    }
                    const float ll = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                    if (ll > 1e-8f) { l[0] /= ll; l[1] /= ll; l[2] /= ll; }
                    if (kind == 2u) {
                        float dir[3] = {reg_float(0x6Fu + 3u * li), reg_float(0x70u + 3u * li), reg_float(0x71u + 3u * li)};
                        const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                        if (dl > 1e-8f) { dir[0] /= dl; dir[1] /= dl; dir[2] /= dl; }
                        const float cosang = -(dir[0] * l[0] + dir[1] * l[1] + dir[2] * l[2]);
                        const float cutoff = reg_float(0x8Bu + li), expo = reg_float(0x87u + li);
                        att *= cosang >= cutoff ? (expo > 0.0f ? std::pow(std::max(cosang, 0.0f), expo) : 1.0f) : 0.0f;
                    }
                    float la[3], ld[3], ls[3];
                    unpack(reg_[0x8Fu + li], la); unpack(reg_[0x93u + li], ld); unpack(reg_[0x97u + li], ls);
                    const float ndl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
                    const float dif = std::max(ndl, 0.0f) * att;
                    for (int r = 0; r < 3; ++r) { amb_sum[r] += la[r] * att; diff_sum[r] += ld[r] * mat_d[r] * dif; }
                    if (comp != 0u && ndl > 0.0f) {
                        float h[3] = {l[0], l[1], l[2] + 1.0f};
                        const float hl = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
                        if (hl > 1e-8f) { h[0] /= hl; h[1] /= hl; h[2] /= hl; }
                        const float ndh = std::max(n[0] * h[0] + n[1] * h[1] + n[2] * h[2], 0.0f);
                        const float sp = std::pow(ndh, spec_pow) * att;
                        for (int r = 0; r < 3; ++r) spec_sum[r] += ls[r] * mat_s[r] * sp;
                    }
                }
                float rgb[3];
                for (int r = 0; r < 3; ++r) rgb[r] = std::clamp(emis[r] + amb_sum[r] * mat_a[r] + diff_sum[r] + spec_sum[r], 0.0f, 1.0f);
                gv.r = rgb[0] * 255.0f; gv.g = rgb[1] * 255.0f; gv.b = rgb[2] * 255.0f; gv.a = mat_alpha * static_cast<float>(reg_[0x5D] & 255u) / 255.0f;   // scene ambient alpha scales the material alpha (the billboard fade ramps it)
                {
                    // Diagnostic (RR_LIGHT_LOG=1): lit result of the first vertex of sky cloud/glare quads (blend dst-colour * 1).
                    static const bool log_on = std::getenv("RR_LIGHT_LOG") != nullptr; static int logged = 0; static std::uint64_t last_frame = ~0ull;
                    if (log_on && i < 12u && draw.blend && draw.blend_src == 0u && draw.blend_dst == 10u && logged < 2500 && (frame_ % 80u == 0u) && !(i != 0u && last_frame != frame_)) {
                        ++logged; if (i == 0u) last_frame = frame_;
                        char lb[1400];
                        int o = std::snprintf(lb, sizeof lb, "GELITCLOUD frame=%llu vtx=%u upd=%u vcol=%d in=%08X out=%.0f,%.0f,%.0f a=%.0f mat_a=%.2f,%.2f,%.2f mat_d=%.2f,%.2f,%.2f emis=%.2f,%.2f,%.2f scene=%.2f,%.2f,%.2f nrm=%.2f,%.2f,%.2f(len %.2f) ",
                            static_cast<unsigned long long>(frame_), i, update, vc ? 1 : 0, rv.color, gv.r, gv.g, gv.b, gv.a, mat_a[0], mat_a[1], mat_a[2], mat_d[0], mat_d[1], mat_d[2],
                            emis[0], emis[1], emis[2], scene_a[0], scene_a[1], scene_a[2], n[0], n[1], n[2], nl);
                        for (std::uint32_t li = 0; li < 4u; ++li)
                            if (reg_[0x18u + li] & 1u)
                                o += std::snprintf(lb + o, sizeof lb - static_cast<std::size_t>(o), "L%u[type=%03X pos=%.2f,%.2f,%.2f dir=%.2f,%.2f,%.2f amb=%06X dif=%06X spc=%06X k=%g,%g,%g] ", li, reg_[0x5Fu + li] & 0xFFFu,
                                    reg_float(0x63u + 3u * li), reg_float(0x64u + 3u * li), reg_float(0x65u + 3u * li), reg_float(0x6Fu + 3u * li), reg_float(0x70u + 3u * li), reg_float(0x71u + 3u * li),
                                    reg_[0x8Fu + li] & 0xFFFFFFu, reg_[0x93u + li] & 0xFFFFFFu, reg_[0x97u + li] & 0xFFFFFFu,
                                    static_cast<double>(reg_float(0x7Bu + 3u * li)), static_cast<double>(reg_float(0x7Cu + 3u * li)), static_cast<double>(reg_float(0x7Du + 3u * li)));
                        log_line(category::kGe, lb);
                    }
                }
            }
            // texture coordinates
            float s = rv.uv[0], t = rv.uv[1];
            if (tex_map_mode == 0u) {
                s = s * scale_u + off_u;
                t = t * scale_v + off_v;
            } else if (tex_map_mode == 1u) {
                // Texture matrix; source selected by the projection mode (UV, position, normal, normalised normal).
                const std::uint32_t src = (reg_[0xC0] >> 8u) & 3u;
                float in[3] = {rv.uv[0] * scale_u + off_u, rv.uv[1] * scale_v + off_v, 0.0f};
                if (src == 1u) { in[0] = p[0]; in[1] = p[1]; in[2] = p[2]; }
                else if (src >= 2u) {
                    in[0] = rv.nrm[0]; in[1] = rv.nrm[1]; in[2] = rv.nrm[2];
                    if (src == 2u) {
                        const float len = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
                        if (len > 1e-8f) { in[0] /= len; in[1] /= len; in[2] /= len; }
                    }
                }
                const float *m = tex_matrix_.data();
                s = m[0] * in[0] + m[3] * in[1] + m[6] * in[2] + m[9];
                t = m[1] * in[0] + m[4] * in[1] + m[7] * in[2] + m[10];
            } else if (tex_map_mode == 2u) {
                // Environment (shade) mapping: UV from the dot products of the world-space normal with two lights.
                float n[3];
                for (int r = 0; r < 3; ++r) n[r] = world_[r] * rv.nrm[0] + world_[3 + r] * rv.nrm[1] + world_[6 + r] * rv.nrm[2];
                const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (nl > 1e-8f) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
                const auto light_dot = [&](std::uint32_t index) {
                    float l[3] = {reg_float(0x63u + 3u * index), reg_float(0x64u + 3u * index), reg_float(0x65u + 3u * index)};
                    const float ll = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                    if (ll > 1e-8f) { l[0] /= ll; l[1] /= ll; l[2] /= ll; }
                    return 0.5f + 0.5f * (n[0] * l[0] + n[1] * l[1] + n[2] * l[2]);
                };
                s = light_dot(reg_[0xC1] & 3u);
                t = light_dot((reg_[0xC1] >> 8u) & 3u);
            }
            gv.u = s * tex_w;
            gv.v = t * tex_h;
        }
        clip.push_back(c);
    }
    if (index_unit == 0u) vertex_addr_ = vbase + count * vt.size;
    else index_addr_ += count * index_unit;

    sub_scope.emplace(prof::kPrimAssemble);
    scratch_.clear();
    const auto to_screen = [&](const Clip &c) {
        GpuVertex v = c.v;
        const float inv_w = 1.0f / c.w;
        v.x = c.x * inv_w * vp_xs + vp_xc - scr_off_x;
        v.y = c.y * inv_w * vp_ys + vp_yc - scr_off_y;
        v.z = c.z * inv_w * vp_zs + vp_zc;
        v.inv_w = inv_w;
        return v;
    };
    const std::uint32_t cull_enable = reg_[0x1D] & 1u;
    const std::uint32_t cull_mode = reg_[0x9B] & 1u;
    const bool want_full = !vt.through && backend_.wants_full_triangles();
    full_.clear();
    const auto emit_triangle = [&](const Clip &a, const Clip &b, const Clip &c) {
        if (vt.through) {
            scratch_.push_back(a.v); scratch_.push_back(b.v); scratch_.push_back(c.v);
            return;
        }
        // Fast path: all three vertices inside both planes (almost every triangle): nothing to clip, same result as below.
        if (a.z + a.w >= 0.0f && a.w - a.z >= 0.0f && b.z + b.w >= 0.0f && b.w - b.z >= 0.0f && c.z + c.w >= 0.0f && c.w - c.z >= 0.0f) {
            if (!(a.w > 1e-7f) || !(b.w > 1e-7f) || !(c.w > 1e-7f)) return;
            const GpuVertex s0 = to_screen(a), s1 = to_screen(b), s2 = to_screen(c);
            if (want_full) { full_.push_back(s0); full_.push_back(s1); full_.push_back(s2); }
            if (cull_enable && !cull_disabled()) {
                const float area = (s1.x - s0.x) * (s2.y - s0.y) - (s2.x - s0.x) * (s1.y - s0.y);
                if ((cull_mode == 1u && area < 0.0f) || (cull_mode == 0u && area > 0.0f)) { ++stats_.culled; return; }
            }
            scratch_.push_back(s0); scratch_.push_back(s1); scratch_.push_back(s2);
            return;
        }
        // Clip against the near and far planes in homogeneous space.
        Clip poly[10] = {a, b, c};
        int n = 3;
        for (int plane = 0; plane < 2; ++plane) {
            Clip next[10];
            int m = 0;
            const auto dist = [&](const Clip &p) { return plane == 0 ? p.z + p.w : p.w - p.z; };
            for (int i = 0; i < n; ++i) {
                const Clip &p = poly[i], &q = poly[(i + 1) % n];
                const float dp = dist(p), dq = dist(q);
                if (dp >= 0.0f) next[m++] = p;
                if ((dp >= 0.0f) != (dq >= 0.0f)) {
                    const float t = dp / (dp - dq);
                    Clip r{};
                    r.x = p.x + (q.x - p.x) * t; r.y = p.y + (q.y - p.y) * t;
                    r.z = p.z + (q.z - p.z) * t; r.w = p.w + (q.w - p.w) * t;
                    r.v.u = p.v.u + (q.v.u - p.v.u) * t; r.v.v = p.v.v + (q.v.v - p.v.v) * t;
                    r.v.r = p.v.r + (q.v.r - p.v.r) * t; r.v.g = p.v.g + (q.v.g - p.v.g) * t;
                    r.v.b = p.v.b + (q.v.b - p.v.b) * t; r.v.a = p.v.a + (q.v.a - p.v.a) * t; r.v.fog = p.v.fog + (q.v.fog - p.v.fog) * t;
                    if (m < 10) next[m++] = r;
                }
            }
            n = m;
            for (int i = 0; i < n; ++i) poly[i] = next[i];
            if (n < 3) return;
        }
        if (n != 3) ++stats_.clipped;
        GpuVertex sv[10];
        for (int i = 0; i < n; ++i) {
            if (!(poly[i].w > 1e-7f)) return;
            sv[i] = to_screen(poly[i]);
        }
        for (int i = 1; i + 1 < n; ++i) {
            if (want_full) { full_.push_back(sv[0]); full_.push_back(sv[i]); full_.push_back(sv[i + 1]); }
            if (cull_enable && !cull_disabled()) {
                const float area = (sv[i].x - sv[0].x) * (sv[i + 1].y - sv[0].y) -
                                   (sv[i + 1].x - sv[0].x) * (sv[i].y - sv[0].y);
                if ((cull_mode == 1u && area < 0.0f) || (cull_mode == 0u && area > 0.0f)) { ++stats_.culled; continue; }
            }
            scratch_.push_back(sv[0]); scratch_.push_back(sv[i]); scratch_.push_back(sv[i + 1]);
        }
    };

    const auto screen_vertex = [&](const Clip &c) -> GpuVertex {
        if (vt.through) return c.v;
        return to_screen(c);
    };

    GpuPrim kind = GpuPrim::Triangles;
    switch (prim) {
    case 0u:
        kind = GpuPrim::Points;
        for (const Clip &c : clip) if (vt.through || c.w > 1e-7f) scratch_.push_back(screen_vertex(c));
        stats_.points += scratch_.size();
        break;
    case 1u:
        kind = GpuPrim::Lines;
        for (std::size_t i = 0; i + 1 < clip.size(); i += 2) {
            if (!vt.through && !(clip[i].w > 1e-7f && clip[i + 1].w > 1e-7f)) continue;
            scratch_.push_back(screen_vertex(clip[i])); scratch_.push_back(screen_vertex(clip[i + 1]));
        }
        stats_.lines += scratch_.size() / 2u;
        break;
    case 2u:
        kind = GpuPrim::Lines;
        for (std::size_t i = 0; i + 1 < clip.size(); ++i) {
            if (!vt.through && !(clip[i].w > 1e-7f && clip[i + 1].w > 1e-7f)) continue;
            scratch_.push_back(screen_vertex(clip[i])); scratch_.push_back(screen_vertex(clip[i + 1]));
        }
        stats_.lines += scratch_.size() / 2u;
        break;
    case 3u:
        for (std::size_t i = 0; i + 2 < clip.size(); i += 3) emit_triangle(clip[i], clip[i + 1], clip[i + 2]);
        break;
    case 4u:
        for (std::size_t i = 0; i + 2 < clip.size(); ++i) {
            if (i & 1u) emit_triangle(clip[i + 1], clip[i], clip[i + 2]);
            else emit_triangle(clip[i], clip[i + 1], clip[i + 2]);
        }
        break;
    case 5u:
        for (std::size_t i = 1; i + 1 < clip.size(); ++i) emit_triangle(clip[0], clip[i], clip[i + 1]);
        break;
    case 6u:
        kind = GpuPrim::Sprites;
        for (std::size_t i = 0; i + 1 < clip.size(); i += 2) {
            if (!vt.through && !(clip[i].w > 1e-7f && clip[i + 1].w > 1e-7f)) continue;
            scratch_.push_back(screen_vertex(clip[i])); scratch_.push_back(screen_vertex(clip[i + 1]));
        }
        stats_.sprites += scratch_.size() / 2u;
        break;
    default: break;
    }
    if (kind == GpuPrim::Triangles) stats_.triangles += scratch_.size() / 3u;
    sub_scope.reset();
    if (scratch_.empty()) return;
    if (!vt.through) {   // RR_POP_LOG=start,count: on-screen box of every 3D draw (pop-in hunting)
        static long long p0 = -1, pn = 0; static bool pp = false;
        if (!pp) { pp = true; if (const char *e = std::getenv("RR_POP_LOG")) std::sscanf(e, "%lld,%lld", &p0, &pn); }
        const long long f = static_cast<long long>(frame_);
        if (p0 >= 0 && f >= p0 && f < p0 + pn) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e9f;
            for (const auto &v : scratch_) { x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y); z0 = std::min(z0, v.z); }
            char b[200];
            std::snprintf(b, sizeof b, "POP f=%lld c=%08X v=%08X t=%08X n=%zu box=%.0f,%.0f,%.0f,%.0f z=%.0f fb=%08X", f, call_target_, vbase, draw.tex_addr,
                          scratch_.size() / 3u, x0, y0, x1, y1, z0, draw.fb_addr);
            log_line(category::kGe, b);
        }
    }
    {
        bool known = false;
        for (TargetRec &t : targets_)
            if (t.fb_addr == draw.fb_addr) { t.stride = draw.fb_stride; t.format = draw.fb_format; t.frame = frame_; known = true; }
        if (!known && targets_.size() < 24u) targets_.push_back({draw.fb_addr, draw.fb_stride, draw.fb_format, frame_});
    }
    const std::uint64_t purple_before = backend_.diag.purple_pixels;
    backend_.diag.have_sample = false;
    // Diagnostic: detect draws that modify the framebuffer margin (x >= 480), where the game keeps palettes.
    std::uint8_t margin_before[64]{}, margin_after[64]{};
    const std::uint32_t margin_bpp = draw.fb_format == 3u ? 4u : 2u;
    const std::uint32_t margin_addr = draw.fb_addr + 480u * margin_bpp + 12u * draw.fb_stride * margin_bpp;
    static std::uint32_t margin_logged = 0u;
    const bool margin_watch = margin_logged < 60u && frame_ >= 5600u && draw.fb_format != 3u && memory_.contains(margin_addr, 64u);
    if (margin_watch) memory_.copy_out(margin_addr, std::span<std::uint8_t>(margin_before, 64u));
    const auto draw_t0 = std::chrono::steady_clock::now();
    {
        // RR_PIXEL_TRACE=frame,x,y : log every draw whose primitives cover that pixel (in GE frames frame..frame+1).
        static int trace_frame = -2, trace_x = 0, trace_y = 0; static bool parsed = false; static int logged = 0;
        if (!parsed) { parsed = true; if (const char *e = std::getenv("RR_PIXEL_TRACE")) { if (std::sscanf(e, "%d,%d,%d", &trace_frame, &trace_x, &trace_y) != 3) trace_frame = -2; } }
        if (trace_frame >= 0 && static_cast<std::int64_t>(frame_) >= trace_frame && static_cast<std::int64_t>(frame_) <= trace_frame + 1 && logged < 600) {
            const float px = trace_x + 0.5f, py = trace_y + 0.5f;
            const std::size_t per = kind == GpuPrim::Triangles ? 3u : (kind == GpuPrim::Sprites ? 2u : 0u);
            for (std::size_t i = 0; per != 0u && i + per <= scratch_.size(); i += per) {
                float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
                for (std::size_t k = 0; k < per; ++k) {
                    x0 = std::min(x0, scratch_[i + k].x); x1 = std::max(x1, scratch_[i + k].x);
                    y0 = std::min(y0, scratch_[i + k].y); y1 = std::max(y1, scratch_[i + k].y);
                    z0 = std::min(z0, scratch_[i + k].z); z1 = std::max(z1, scratch_[i + k].z);
                }
                if (px < x0 || px > x1 || py < y0 || py > y1) continue;
                ++logged;
                char tl[900];
                std::snprintf(tl, sizeof tl,
                    "GEPIXEL frame=%llu fb=%08X fmt=%u prim=%u n=%zu idx=%zu bbox=(%.0f,%.0f)-(%.0f,%.0f) z=%.0f..%.0f through=%d clear=%d(c%d a%d d%d) tex=%d(%08X f%u %ux%u s%u) "
                    "tfunc=%u talpha=%d blend=%d(%u,%u,%u) atest=%d(%u,%u) depth=%d(f%u w%d) mask=%06X/%02X fog=%d stencil=%d col0=%.0f,%.0f,%.0f,%.0f scis=%d,%d-%d,%d",
                    static_cast<unsigned long long>(frame_), draw.fb_addr, draw.fb_format, static_cast<unsigned>(kind), scratch_.size(), i / per, x0, y0, x1, y1, z0, z1,
                    draw.through ? 1 : 0, draw.clear_mode ? 1 : 0, draw.clear_color ? 1 : 0, draw.clear_alpha ? 1 : 0, draw.clear_depth ? 1 : 0,
                    draw.tex_enable ? 1 : 0, draw.tex_addr, draw.tex_format, draw.tex_width, draw.tex_height, draw.tex_stride,
                    draw.tex_func, draw.tex_alpha ? 1 : 0, draw.blend ? 1 : 0, draw.blend_src, draw.blend_dst, draw.blend_eq,
                    draw.alpha_test ? 1 : 0, draw.atest_func, draw.atest_ref, draw.depth_test ? 1 : 0, draw.depth_func, draw.depth_write ? 1 : 0,
                    draw.mask_rgb, draw.mask_alpha, draw.fog ? 1 : 0, draw.stencil_test ? 1 : 0,
                    scratch_[i].r, scratch_[i].g, scratch_[i].b, scratch_[i].a, draw.scissor_x0, draw.scissor_y0, draw.scissor_x1, draw.scissor_y1);
                {
                    const std::uint32_t bpp = draw.fb_format == 3u ? 4u : 2u;
                    const std::uint32_t pa = draw.fb_addr + (static_cast<std::uint32_t>(trace_y) * draw.fb_stride + static_cast<std::uint32_t>(trace_x)) * bpp;
                    std::uint8_t pv[4]{};
                    backend_.vram_read(pa, bpp);
                    if (memory_.contains(pa, bpp)) memory_.copy_out(pa, std::span<std::uint8_t>(pv, bpp));
                    char pl[160]; std::snprintf(pl, sizeof pl, "GEPIXVAL before-draw fb=%08X px=%02X%02X%02X%02X fogcol=%06X", draw.fb_addr, pv[0], pv[1], pv[2], pv[3], static_cast<unsigned>(draw.fog_color));
                    log_line(category::kGe, pl);
                }
                log_line(category::kGe, tl);
            }
        }
    }
    {
        // RR_BB_COMPACT=1 : one short line per billboard slide quad (any frame): vertex alphas and palette alpha.
        static const bool bbc = std::getenv("RR_BB_COMPACT") != nullptr;
        if (bbc && draw.tex_enable && draw.tex_addr >= 0x04160FC0u && draw.tex_addr <= 0x04170FC0u && scratch_.size() == 6u && draw.tex_width == 128u && draw.tex_height == 64u) {
            std::uint32_t c0 = 0u; if (clut_.size() >= 4u) std::memcpy(&c0, clut_.data(), 4);
            std::uint64_t th = 1469598103934665603ull;
            backend_.vram_read(draw.tex_addr, 4096u);
            if (const std::uint8_t *tp = memory_.raw_pointer(draw.tex_addr, 4096u)) for (std::uint32_t i = 0; i < 4096u; ++i) th = (th ^ tp[i]) * 1099511628211ull;
            std::uint64_t ch = 1469598103934665603ull;
            for (std::size_t i = 0; i < clut_.size(); ++i) ch = (ch ^ clut_[i]) * 1099511628211ull;
            char b[560]; std::snprintf(b, sizeof b, "GEBBC th=%08X ch=%08X f=%llu fb=%08X tex=%08X va=%.0f,%.0f,%.0f,%.0f,%.0f,%.0f vrgb=%.0f,%.0f,%.0f clut0=%08X blend=%d(%u,%u) tf=%u ta=%d bbox=%.0f,%.0f lit=%u upd=%u r53=%06X r54=%06X r55=%06X r56=%06X r57=%06X r58=%02X r5C=%06X r5D=%02X",
                static_cast<unsigned>(th & 0xFFFFFFFFu), static_cast<unsigned>(ch & 0xFFFFFFFFu), static_cast<unsigned long long>(frame_), draw.fb_addr, draw.tex_addr, scratch_[0].a, scratch_[1].a, scratch_[2].a, scratch_[3].a, scratch_[4].a, scratch_[5].a,
                scratch_[0].r, scratch_[0].g, scratch_[0].b, c0, draw.blend ? 1 : 0, draw.blend_src, draw.blend_dst, draw.tex_func, draw.tex_alpha ? 1 : 0, scratch_[0].x, scratch_[0].y,
                reg_[0x17] & 1u, reg_[0x53] & 7u, reg_[0x53] & 0xFFFFFFu, reg_[0x54] & 0xFFFFFFu, reg_[0x55] & 0xFFFFFFu, reg_[0x56] & 0xFFFFFFu, reg_[0x57] & 0xFFFFFFu, reg_[0x58] & 0xFFu, reg_[0x5C] & 0xFFFFFFu, reg_[0x5D] & 0xFFu);
            log_line(category::kGe, b);
        }
    }
    {
        // RR_DRAW_LOG=first,last : one compact line per draw for GE frames first..last (GEDRAW).
        static int dl_a = -1, dl_b = -1, dl_s = 1; static bool dl_parsed = false; static int dl_n = 0;
        if (!dl_parsed) { dl_parsed = true; if (const char *e = std::getenv("RR_DRAW_LOG")) { if (std::sscanf(e, "%d,%d,%d", &dl_a, &dl_b, &dl_s) < 2) dl_a = -1; if (dl_s < 1) dl_s = 1; } }
        if (dl_a >= 0 && static_cast<int>(frame_) >= dl_a && static_cast<int>(frame_) <= dl_b && (static_cast<int>(frame_) - dl_a) % dl_s == 0 && dl_n < 60000 && !scratch_.empty()) {
            ++dl_n;
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (const auto &v : scratch_) { x0 = std::min(x0, v.x); x1 = std::max(x1, v.x); y0 = std::min(y0, v.y); y1 = std::max(y1, v.y); }
            char tl[700];
            std::snprintf(tl, sizeof tl,
                "GEDRAW f=%llu fb=%08X/%u prim=%u n=%zu bbox=(%.0f,%.0f)-(%.0f,%.0f) thr=%d clr=%d%d%d tex=%d:%08X/f%u/%ux%u tf=%u ta=%d bl=%d(%u,%u,%u) fix=%06X/%06X at=%d(%u,%u) dp=%d(f%u,w%d) st=%d(f%u r%02X m%02X op%u%u%u) ct=%d:%u/%06X/%06X mask=%06X/%02X col0=%.0f,%.0f,%.0f,%.0f",
                static_cast<unsigned long long>(frame_), draw.fb_addr, draw.fb_format, static_cast<unsigned>(kind), scratch_.size(), x0, y0, x1, y1,
                draw.through ? 1 : 0, draw.clear_mode ? 1 : 0, draw.clear_color ? 1 : 0, draw.clear_alpha ? 1 : 0,
                draw.tex_enable ? 1 : 0, draw.tex_addr, draw.tex_format, draw.tex_width, draw.tex_height, draw.tex_func, draw.tex_alpha ? 1 : 0,
                draw.blend ? 1 : 0, draw.blend_src, draw.blend_dst, draw.blend_eq, reg_[0xDF] & 0xFFFFFFu, reg_[0xE0] & 0xFFFFFFu,
                draw.alpha_test ? 1 : 0, draw.atest_func, draw.atest_ref, draw.depth_test ? 1 : 0, draw.depth_func, draw.depth_write ? 1 : 0,
                draw.stencil_test ? 1 : 0, draw.stencil_func, draw.stencil_ref, draw.stencil_mask, draw.stencil_sfail, draw.stencil_zfail, draw.stencil_zpass,
                static_cast<int>(reg_[0x26] & 1u), reg_[0xD8] & 3u, reg_[0xD9] & 0xFFFFFFu, reg_[0xDA] & 0xFFFFFFu, draw.mask_rgb, draw.mask_alpha,
                scratch_[0].r, scratch_[0].g, scratch_[0].b, scratch_[0].a);
            log_line(category::kGe, tl);
            if (draw.fb_addr == 0x04195140u) {
                // Palette buffer as the CPU-visible VRAM holds it right now (no GPU readback): is the alpha ramp there?
                if (const std::uint8_t *pp = memory_.raw_pointer(0x04195140u, 1024u)) {
                    std::string ps = "GECLUTPRE vram-before-sprite:";
                    for (int i : {0, 1, 2, 8, 32, 64, 96, 128, 160, 200, 255}) { std::uint32_t c; std::memcpy(&c, pp + i * 4, 4); char b2[24]; std::snprintf(b2, sizeof b2, " %d=%08X", i, c); ps += b2; }
                    log_line(category::kGe, ps);
                }
            }
            if (draw.tex_enable && ((draw.tex_addr >= 0x04161FC0u && draw.tex_addr <= 0x04165FC0u) || draw.tex_addr == 0x040FC7C0u) && scratch_.size() <= 6u && std::getenv("RR_BB_LOG") != nullptr) {
                backend_.vram_read(draw.tex_addr, 4096u);
                std::uint64_t th = 1469598103934665603ull;
                if (const std::uint8_t *tp = memory_.raw_pointer(draw.tex_addr, 4096u)) for (std::uint32_t i = 0; i < 4096u; ++i) th = (th ^ tp[i]) * 1099511628211ull;
                std::string bs = "GEBB addr=" + [&] { char b[48]; std::snprintf(b, sizeof b, "%08X tex=%016llX", draw.tex_addr, static_cast<unsigned long long>(th)); return std::string(b); }();
                { char b[160]; std::snprintf(b, sizeof b, " clut=%016llX src=%08X start=%u shift=%u mask=%02X", static_cast<unsigned long long>(clut_hash_), clut_src_, reg_[0xC3] & 0xFFu, (reg_[0xC5] >> 2u) & 31u, (reg_[0xC5] >> 8u) & 0xFFu); bs += b; }
                { bs += " clutE:"; for (std::uint32_t i = 0; i < 16u && (i + 1u) * 4u <= clut_.size(); ++i) { std::uint32_t c; std::memcpy(&c, clut_.data() + i * 4u, 4); char b[16]; std::snprintf(b, sizeof b, " %08X", c); bs += b; } }
                for (std::size_t vi = 0; vi < scratch_.size(); ++vi) {
                    char vb[120]; std::snprintf(vb, sizeof vb, " [%zu](%.1f,%.1f uv=%.1f,%.1f c=%.0f,%.0f,%.0f,%.0f)", vi, scratch_[vi].x, scratch_[vi].y, scratch_[vi].u, scratch_[vi].v, scratch_[vi].r, scratch_[vi].g, scratch_[vi].b, scratch_[vi].a);
                    bs += vb;
                }
                log_line(category::kGe, bs);
            }
            if (draw.fb_addr == 0x041FC000u || draw.fb_addr == 0x04195140u || draw.fb_addr == 0x04027BC0u || draw.fb_addr == 0x04034FC0u || draw.fb_addr == 0x0403BBC0u || (draw.tex_enable && draw.tex_addr == 0x04195540u)) {
                std::string vs = std::string("GEDRAWV vtype=") + [&] { char b[16]; std::snprintf(b, sizeof b, "%08X", reg_[0x12]); return std::string(b); }();
                for (std::size_t vi = 0; vi < scratch_.size() && vi < 70u; vi += (scratch_.size() > 8u && !(draw.tex_enable && draw.tex_addr == 0x04195540u) ? 7u : 1u)) {
                    char vb[120]; std::snprintf(vb, sizeof vb, " [%zu](%.1f,%.1f uv=%.1f,%.1f c=%.0f,%.0f,%.0f,%.0f)", vi, scratch_[vi].x, scratch_[vi].y, scratch_[vi].u, scratch_[vi].v, scratch_[vi].r, scratch_[vi].g, scratch_[vi].b, scratch_[vi].a);
                    vs += vb;
                }
                log_line(category::kGe, vs);
            }
        }
    }
    {
        // Debug: RR_SKIP=glow,cloud drops the final glow composite and/or the sky cloud layers (diagnosis of washed-out sky).
        static bool parsed = false, skip_glow = false, skip_cloud = false, skip_glare = false;
        if (!parsed) {
            parsed = true;
            if (const char *e = std::getenv("RR_SKIP")) { const std::string t = e; skip_glow = t.find("glow") != std::string::npos; skip_cloud = t.find("cloud") != std::string::npos; skip_glare = t.find("glare") != std::string::npos; }
        }
        if (skip_glow && draw.through && draw.tex_enable && draw.blend && draw.blend_src == 10u && draw.blend_dst == 10u && draw.fb_format == 1u && draw.tex_format == 3u) return;
        if (skip_glare && draw.tex_enable && draw.tex_addr == 0x04195540u) return;
        if (draw.tex_enable && draw.tex_addr == 0x04195540u) {
            // RR_GLARE_DUMP=frame : write the glare texture (raw, swizzled) and its palette to glare_tex_<f>.bin / glare_clut_<f>.bin.
            static const char *dump_env = std::getenv("RR_GLARE_DUMP");
            static std::uint64_t dumped_frame = ~0ull; static int dumped = 0;
            if (dump_env != nullptr && frame_ >= static_cast<std::uint64_t>(std::atoll(dump_env)) && frame_ != dumped_frame && dumped < 3) {
                dumped_frame = frame_; ++dumped;
                backend_.vram_read(0x04195540u, 16384u);
                if (const std::uint8_t *tp = memory_.raw_pointer(0x04195540u, 16384u)) {
                    const std::string f = "glare_tex_" + std::to_string(frame_) + ".bin";
                    if (FILE *fp = std::fopen(f.c_str(), "wb")) { std::fwrite(tp, 1, 16384, fp); std::fclose(fp); }
                }
                const std::string f2 = "glare_clut_" + std::to_string(frame_) + ".bin";
                if (FILE *fp = std::fopen(f2.c_str(), "wb")) { std::fwrite(draw.clut, 1, 1024, fp); std::fclose(fp); }
            }
        }
        if (draw.tex_enable && draw.tex_addr == 0x04195540u && std::getenv("RR_GLARE_LOG") != nullptr) {
            static int gl_n = 0;
            bool any_nz = false; for (int i = 0; i < 256 && !any_nz; ++i) { std::uint32_t c; std::memcpy(&c, draw.clut + i * 4, 4); any_nz = c != 0; }
            if (gl_n < 120 && any_nz) {
                ++gl_n;
                std::string out = "GEGLARE f=" + std::to_string(frame_) + " clutfmt=" + std::to_string(draw.clut_format) + " shift=" + std::to_string(draw.clut_shift) + " mask=" + std::to_string(draw.clut_mask) + " start=" + std::to_string(draw.clut_start) + " clut:";
                std::uint32_t nz = 0, mx = 0;
                for (int i = 0; i < 256; ++i) {
                    std::uint32_t c; std::memcpy(&c, draw.clut + i * 4, 4);
                    if (c != 0) ++nz;
                    mx = std::max(mx, std::max({c & 255u, (c >> 8) & 255u, (c >> 16) & 255u}));
                    if (i < 24 || i >= 240 || i % 16 == 0) { char b[24]; std::snprintf(b, sizeof b, " %d=%08X", i, c); out += b; }
                }
                out += " nz=" + std::to_string(nz) + " maxch=" + std::to_string(mx);
                const std::uint8_t *row = memory_.raw_pointer(0x041FC000u, 2048u);
                if (row != nullptr) {
                    std::uint32_t rnz = 0; for (int i = 0; i < 512; ++i) { std::uint32_t c; std::memcpy(&c, row + i * 4, 4); if (c) ++rnz; }
                    std::uint32_t p0; std::memcpy(&p0, row, 4);
                    out += " row041FC000 nz=" + std::to_string(rnz) + " px0=" + [&]{ char b[12]; std::snprintf(b, sizeof b, "%08X", p0); return std::string(b); }();
                }
                const std::uint8_t *td = memory_.raw_pointer(0x04195540u, 16384u);
                if (td != nullptr) { out += " texbytes:"; for (int i = 0; i < 16384; i += 256) { char b[6]; std::snprintf(b, sizeof b, "%02X", td[i]); out += b; } }
                log_line(category::kGe, out);
            }
        }
        if (skip_cloud && !draw.through && draw.blend && draw.blend_src == 0u && draw.blend_dst == 10u) return;
    }
    {
        const prof::Scope prof_scope(prof::kBackendDraw);
        static const double draw_time_ms = [] { const char *e = std::getenv("RR_DRAW_TIME"); return e ? std::atof(e) : -1.0; }();
        const bool glare_time = draw.tex_enable && draw.tex_addr == 0x04195540u && std::getenv("RR_GLARE_TIME") != nullptr;
        const bool timed = glare_time || draw_time_ms >= 0.0;
        const auto t0 = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        static const bool textime = std::getenv("RR_TEXTIME") != nullptr;
        if (textime && draw.tex_enable && !draw.through) tex_cur_[draw.tex_addr] = (draw.tex_width << 16) | (draw.tex_height & 0xffffu);
        {   // RR_TEXUV="lo,hi;start,count": per draw with a texture in [lo,hi), log uv range, first vertex colour and blend (billboard animation)
            static unsigned tl = 0, th = 0; static long long us = 0, uc = 0; static bool up = false; static int ul = 0;
            if (!up) { up = true; if (const char *e = std::getenv("RR_TEXUV")) std::sscanf(e, "%x,%x;%lld,%lld", &tl, &th, &us, &uc); }
            const long long f = static_cast<long long>(frame_);
            if (th != 0u && draw.tex_enable && draw.tex_addr >= tl && draw.tex_addr < th && f >= us && f < us + uc && ul < 20000 && !scratch_.empty()) {
                float u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f;
                for (const GpuVertex &v : scratch_) { u0 = std::min(u0, v.u); v0 = std::min(v0, v.v); u1 = std::max(u1, v.u); v1 = std::max(v1, v.v); }
                const GpuVertex &q = scratch_.front();
                char b[256];
                std::snprintf(b, sizeof b, "TEXUV f=%lld tex=%08X n=%zu u=%.2f..%.2f v=%.2f..%.2f rgba=%.0f,%.0f,%.0f,%.0f blend=%d(%u,%u) fix=%08X/%08X",
                              f, draw.tex_addr, scratch_.size(), u0, u1, v0, v1, q.r, q.g, q.b, q.a, draw.blend ? 1 : 0, draw.blend_src, draw.blend_dst,
                              draw.fix_a, draw.fix_b);
                log_line(category::kGe, b); ++ul;
            }
        }
        if (want_full && kind == GpuPrim::Triangles && !full_.empty())
            backend_.record_full(draw, full_.data(), full_.size(), (cull_enable && !cull_disabled()) ? static_cast<int>(cull_mode) : -1);
        backend_.draw(draw, kind, scratch_.data(), scratch_.size());
        if (timed) {
            // RR_DRAW_TIME=ms: log every draw call whose CPU time (including any batch flush it triggers) exceeds ms.
            // RR_GLARE_TIME=1: time of each sun-glare draw call.
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            static int n = 0;
            if ((glare_time || ms >= draw_time_ms) && n < 1500) {
                ++n;
                char b[320];
                std::snprintf(b, sizeof b, "GEDRAWTIME f=%llu %.2fms fb=%08X/%u tex=%d:%08X %ux%u through=%d blend=%d(%u,%u) depth=%d clear=%d scissor=%d,%d-%d,%d bytes=%zu",
                              static_cast<unsigned long long>(frame_), ms, draw.fb_addr, draw.fb_format, draw.tex_enable ? 1 : 0, draw.tex_addr, draw.tex_width, draw.tex_height,
                              draw.through ? 1 : 0, draw.blend ? 1 : 0, draw.blend_src, draw.blend_dst, draw.depth_test ? 1 : 0, draw.clear_mode ? 1 : 0,
                              draw.scissor_x0, draw.scissor_y0, draw.scissor_x1, draw.scissor_y1, scratch_.size());
                log_line(category::kGe, b);
            }
        }
    }
    if (margin_watch) {
        memory_.copy_out(margin_addr, std::span<std::uint8_t>(margin_after, 64u));
        if (std::memcmp(margin_before, margin_after, 64u) != 0) {
            ++margin_logged;
            char ml[900];
            std::snprintf(ml, sizeof ml,
                "GEMARGIN frame=%llu fb=%08X stride=%u fmt=%u prim=%u verts=%zu through=%d clear=%d(c%d a%d d%d) scissor=%d,%d-%d,%d "
                "maskrgb=%06X maskalpha=%02X blend=%d tex=%d first=%08X->%08X v0=(%.0f,%.0f) v1=(%.0f,%.0f)",
                static_cast<unsigned long long>(frame_), draw.fb_addr, draw.fb_stride, draw.fb_format, static_cast<unsigned>(kind),
                scratch_.size(), draw.through ? 1 : 0, draw.clear_mode ? 1 : 0, draw.clear_color ? 1 : 0,
                draw.clear_alpha ? 1 : 0, draw.clear_depth ? 1 : 0, draw.scissor_x0, draw.scissor_y0, draw.scissor_x1,
                draw.scissor_y1, draw.mask_rgb, draw.mask_alpha, draw.blend ? 1 : 0, draw.tex_enable ? 1 : 0,
                *reinterpret_cast<std::uint32_t *>(margin_before), *reinterpret_cast<std::uint32_t *>(margin_after),
                scratch_[0].x, scratch_[0].y, scratch_.size() > 1u ? scratch_[1].x : 0.0f, scratch_.size() > 1u ? scratch_[1].y : 0.0f);
            std::string mfull = ml;
            for (std::size_t vi = 0; vi < scratch_.size() && vi < 16u; ++vi) {
                char vb[96];
                std::snprintf(vb, sizeof vb, " [%zu](%.1f,%.1f uv=%.1f,%.1f)", vi, scratch_[vi].x, scratch_[vi].y, scratch_[vi].u, scratch_[vi].v);
                mfull += vb;
            }
            char tb[200];
            std::snprintf(tb, sizeof tb, " vtype=%08X texsize=%ux%u texaddr=%08X clamp=%u offset=%d,%d",
                          reg_[0x12], draw.tex_width, draw.tex_height, draw.tex_addr, reg_[0xC7] & 0xFFu,
                          static_cast<int>(reg_[0x4C] & 0xFFFFu), static_cast<int>(reg_[0x4D] & 0xFFFFu));
            mfull += tb;
            log_line(category::kGe, mfull);
        }
    }
    draw_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - draw_t0).count();
    const std::uint64_t purple_added = backend_.diag.purple_pixels - purple_before;
    frame_purple_ += purple_added;
    if (purple_frame_logged_for_ != frame_) { purple_frame_logged_for_ = frame_; purple_in_frame_ = 0u; }
    if (purple_added >= 150u && purple_draws_logged_ < 800u && purple_in_frame_ < 12u) {
        ++purple_draws_logged_;
        ++purple_in_frame_;
        const GpuDiag &g = backend_.diag;
        char line[1200];
        std::snprintf(line, sizeof line,
            "GEPURPLEDRAW frame=%llu pixels=%llu prim=%u verts=%zu through=%d lit=%d tex=%d addr=%08X fmt=%u size=%ux%u stride=%u swz=%d lin=%d "
            "clutsrc=%08X cluthash=%016llX clutfmt=%u start=%u shift=%u mask=%02X tfunc=%u talpha=%d tdbl=%d blend=%d bs=%u bd=%u fixa=%06X fixb=%06X atest=%d "
            "texel=%08X vcol=%.0f,%.0f,%.0f uv=%.1f,%.1f out=%d,%d,%d v0col=%.0f,%.0f,%.0f fb=%08X "
            "vtcol=%u nrm=%u amb55=%06X emis54=%06X scene5C=%06X lights=%d%d%d%d texmap=%u",
            static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(purple_added), static_cast<unsigned>(kind),
            scratch_.size(), draw.through ? 1 : 0, lighting ? 1 : 0, draw.tex_enable ? 1 : 0, draw.tex_addr, draw.tex_format,
            draw.tex_width, draw.tex_height, draw.tex_stride, draw.tex_swizzled ? 1 : 0, draw.tex_linear ? 1 : 0,
            clut_src_, static_cast<unsigned long long>(clut_hash_), draw.clut_format, draw.clut_start, draw.clut_shift,
            draw.clut_mask, draw.tex_func, draw.tex_alpha ? 1 : 0, draw.tex_double ? 1 : 0, draw.blend ? 1 : 0,
            draw.blend_src, draw.blend_dst, draw.fix_a, draw.fix_b, draw.alpha_test ? 1 : 0,
            g.sample_texel, g.sample_vr, g.sample_vg, g.sample_vb, g.sample_u, g.sample_v,
            g.sample_out_r, g.sample_out_g, g.sample_out_b,
            scratch_[0].r, scratch_[0].g, scratch_[0].b, draw.fb_addr,
            vt.col, vt.nrm, reg_[0x55] & 0xFFFFFFu, reg_[0x54] & 0xFFFFFFu, reg_[0x5C] & 0xFFFFFFu,
            static_cast<int>(reg_[0x18] & 1u), static_cast<int>(reg_[0x19] & 1u), static_cast<int>(reg_[0x1A] & 1u),
            static_cast<int>(reg_[0x1B] & 1u), reg_[0xC0] & 3u);
        std::string full = line;
        full += " tex_prov:" + provenance(draw.tex_addr, 16u);
        full += " clut_prov:" + provenance(clut_src_, 1024u);
        log_line(category::kGe, full);
    }
}

std::string GeProcessor::brief() const {
    std::ostringstream out;
    out << "lists=" << stats_.lists << " prims=" << stats_.prims << " vertices=" << stats_.vertices
        << " clears=" << stats_.clears << " triangles=" << stats_.triangles << " sprites=" << stats_.sprites
        << " textured=" << stats_.textured << " culled=" << stats_.culled << " clipped=" << stats_.clipped;
    return out.str();
}

void GeProcessor::report() const {
    std::ostringstream out;
    out << "GE: lists=" << stats_.lists << " finishes=" << stats_.finishes << " signals=" << stats_.signals
        << " commands=" << stats_.commands << " prims=" << stats_.prims << " vertices=" << stats_.vertices
        << " clears=" << stats_.clears;
    log_line(category::kGe, out.str());
    out.str("");
    out << "GE: triangles=" << stats_.triangles << " sprites=" << stats_.sprites << " lines=" << stats_.lines
        << " points=" << stats_.points << " clipped=" << stats_.clipped << " culled=" << stats_.culled
        << " textured=" << stats_.textured << " through=" << stats_.through << " skinned=" << stats_.skinned;
    log_line(category::kGe, out.str());
    out.str("");
    out << "GE: UNSUPPORTED lit_prims=" << stats_.lit_prims << " bezier/spline=" << stats_.unsupported_bezier
        << " envmap_texgen=" << stats_.unsupported_texgen;
    log_line(category::kGe, out.str());
}

// RR_TEXTIME="start,count" (presented frames, absolute): log when a texture starts / stops being used by 3D draws
// ("TEXON f=N addr WxH" / "TEXOFF f=N addr").  Animated billboards show up as textures that switch on and off periodically.
void GeProcessor::set_frame(std::uint64_t frame) {
    static long long ts = -1, tc = 0; static bool parsed = false; static int lines = 0;
    if (!parsed) { parsed = true; if (const char *e = std::getenv("RR_TEXTIME")) std::sscanf(e, "%lld,%lld", &ts, &tc); }
    if (frame != frame_ && ts >= 0) {
        const long long f = static_cast<long long>(frame_);
        if (f >= ts && f < ts + tc && lines < 20000) {
            for (const auto &[a, wh] : tex_cur_)
                if (!tex_prev_.count(a)) { char b[96]; std::snprintf(b, sizeof b, "TEXON f=%lld %08X %ux%u", f, a, wh >> 16, wh & 0xffffu); log_line(category::kGe, b); ++lines; }
            for (const auto &[a, wh] : tex_prev_)
                if (!tex_cur_.count(a)) { char b[64]; std::snprintf(b, sizeof b, "TEXOFF f=%lld %08X", f, a); log_line(category::kGe, b); ++lines; }
        }
        tex_prev_.swap(tex_cur_);
        tex_cur_.clear();
    }
    {   // RR_TRI_LOG=start,count: triangles / vertices / prims / culled drawn in each of those frames (LOD experiments)
        static long long t0 = -1, tn = 0; static bool tp = false;
        static std::uint64_t last_tri = 0, last_vtx = 0, last_prim = 0, last_cull = 0;
        if (!tp) { tp = true; if (const char *e = std::getenv("RR_TRI_LOG")) std::sscanf(e, "%lld,%lld", &t0, &tn); }
        if (frame != frame_ && t0 >= 0) {
            const long long f = static_cast<long long>(frame_);
            if (f >= t0 && f < t0 + tn) {
                char b[160];
                std::snprintf(b, sizeof b, "TRIS f=%lld tris=%llu verts=%llu prims=%llu culled=%llu", f,
                              static_cast<unsigned long long>(stats_.triangles - last_tri), static_cast<unsigned long long>(stats_.vertices - last_vtx),
                              static_cast<unsigned long long>(stats_.prims - last_prim), static_cast<unsigned long long>(stats_.culled - last_cull));
                log_line(category::kGe, b);
            }
            last_tri = stats_.triangles; last_vtx = stats_.vertices; last_prim = stats_.prims; last_cull = stats_.culled;
        }
    }
    frame_ = frame;
}

} // namespace rr

