#include "rr_iso.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace rr {
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kSector = 2048;

std::uint32_t le32(const unsigned char *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A name from the image becomes a path component on the player's disk: refuse anything that could leave dest_dir.
bool safe_component(const std::string &n) {
    if (n.empty() || n == "." || n == "..") return false;
    return n.find_first_of("\\/:*?\"<>|") == std::string::npos;
}

} // namespace

bool IsoImage::open(const fs::path &file, std::string &err) {
    entries_.clear();
    in_.close();
    in_.clear();
    in_.open(file, std::ios::binary);
    if (!in_) { err = "The disc image could not be opened."; return false; }
    unsigned char pvd[kSector] = {};
    in_.seekg(static_cast<std::streamoff>(16) * kSector);
    in_.read(reinterpret_cast<char *>(pvd), kSector);
    if (in_.gcount() != static_cast<std::streamsize>(kSector) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) {
        err = "This is not a standard ISO disc image (.cso/.pbp files are not supported).";
        return false;
    }
    const unsigned char *root = pvd + 156;
    scan_dir(le32(root + 2), le32(root + 10), "", 0);
    if (entries_.empty()) { err = "The disc image has no files."; return false; }
    return true;
}

void IsoImage::scan_dir(std::uint32_t lba, std::uint32_t size, const std::string &prefix, int depth) {
    if (depth > 16 || size == 0 || size > 64u * 1024u * 1024u) return;
    std::vector<unsigned char> buf(size);
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(lba) * kSector);
    in_.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(size));
    if (in_.gcount() != static_cast<std::streamsize>(size)) return;
    std::size_t pos = 0;
    while (pos < size) {
        const unsigned char len = buf[pos];
        if (len == 0) { pos = (pos / kSector + 1) * kSector; continue; }   // records never cross a sector
        if (pos + len > size || len < 34) break;
        const unsigned char *r = buf.data() + pos;
        const unsigned char name_len = r[32];
        if (33u + name_len <= len && !(name_len == 1 && (r[33] == 0 || r[33] == 1))) {
            std::string name(reinterpret_cast<const char *>(r + 33), name_len);
            if (const auto semi = name.find(';'); semi != std::string::npos) name.resize(semi);
            if (!name.empty() && name.back() == '.') name.pop_back();
            if (safe_component(name)) {
                IsoEntry e;
                e.path = prefix.empty() ? name : prefix + "/" + name;
                e.lba = le32(r + 2);
                e.size = le32(r + 10);
                e.is_dir = (r[25] & 2u) != 0u;
                entries_.push_back(e);
                if (e.is_dir) scan_dir(e.lba, static_cast<std::uint32_t>(e.size), e.path, depth + 1);
            }
        }
        pos += len;
    }
}

const IsoEntry *IsoImage::find(const std::string &path) const {
    const std::string want = lower(path);
    for (const auto &e : entries_)
        if (lower(e.path) == want) return &e;
    return nullptr;
}

std::vector<unsigned char> IsoImage::read_head(const IsoEntry &e, std::size_t n) {
    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(n, e.size));
    std::vector<unsigned char> out(count);
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(e.lba) * kSector);
    in_.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(count));
    out.resize(static_cast<std::size_t>(std::max<std::streamsize>(in_.gcount(), 0)));
    return out;
}

bool IsoImage::extract(const IsoEntry &e, const fs::path &dest, std::string &err,
                       const std::function<void(std::uint64_t)> &bytes_done) {
    std::error_code ec;
    if (!dest.parent_path().empty()) fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) { err = "Could not create " + dest.string(); return false; }
    std::vector<char> chunk(1u << 20);
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(e.lba) * kSector);
    std::uint64_t left = e.size;
    while (left > 0) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(left, chunk.size()));
        in_.read(chunk.data(), static_cast<std::streamsize>(want));
        if (in_.gcount() != static_cast<std::streamsize>(want)) { err = "The disc image is cut off (" + e.path + ")."; return false; }
        out.write(chunk.data(), static_cast<std::streamsize>(want));
        if (!out) { err = "Writing failed (disk full?): " + dest.string(); return false; }
        left -= want;
        if (bytes_done) bytes_done(want);
    }
    return true;
}

bool iso_extract_all(const fs::path &iso, const fs::path &dest_dir, std::string &err, const IsoProgress &progress) {
    IsoImage img;
    if (!img.open(iso, err)) return false;
    std::uint64_t total = 0, done = 0;
    for (const auto &e : img.entries())
        if (!e.is_dir) total += e.size;
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    for (const auto &e : img.entries()) {
        if (e.is_dir) { fs::create_directories(dest_dir / fs::path(e.path), ec); continue; }
        if (!img.extract(e, dest_dir / fs::path(e.path), err, [&](std::uint64_t n) {
                done += n;
                if (progress) progress(done, total);
            }))
            return false;
    }
    return true;
}

bool iso_extract_file(const fs::path &iso, const std::string &inner_path, const fs::path &dest_file, std::string &err) {
    IsoImage img;
    if (!img.open(iso, err)) return false;
    const IsoEntry *e = img.find(inner_path);
    if (!e || e->is_dir) { err = inner_path + " was not found in the disc image."; return false; }
    return img.extract(*e, dest_file, err);
}

} // namespace rr
