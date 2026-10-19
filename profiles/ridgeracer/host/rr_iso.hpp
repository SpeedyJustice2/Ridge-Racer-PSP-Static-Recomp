#pragma once
// Reads a standard ISO 9660 disc image (the kind a PSP game dump is) and copies files out of it.
// Nothing here knows about encryption: it copies bytes.  Platform independent (std::filesystem only).

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace rr {

struct IsoEntry {
    std::string path;          // "PSP_GAME/SYSDIR/BOOT.BIN" (no leading slash, no ";1" version suffix)
    std::uint32_t lba = 0;     // first sector
    std::uint64_t size = 0;    // bytes
    bool is_dir = false;
};

class IsoImage {
public:
    // False with a short reason in err when the file is not a readable ISO 9660 image.
    bool open(const std::filesystem::path &file, std::string &err);
    const std::vector<IsoEntry> &entries() const { return entries_; }
    // Case-insensitive; path uses '/'.  nullptr when absent.
    const IsoEntry *find(const std::string &path) const;
    // First n bytes of a file (fewer when the file is shorter).
    std::vector<unsigned char> read_head(const IsoEntry &e, std::size_t n);
    // Copy one entry to dest (parent folders are created).
    bool extract(const IsoEntry &e, const std::filesystem::path &dest, std::string &err,
                 const std::function<void(std::uint64_t)> &bytes_done = {});

private:
    void scan_dir(std::uint32_t lba, std::uint32_t size, const std::string &prefix, int depth);
    std::ifstream in_;
    std::vector<IsoEntry> entries_;
};

using IsoProgress = std::function<void(std::uint64_t done, std::uint64_t total)>;

// Copy every file of the image into dest_dir (keeps the folder layout).  Returns false and sets err on failure.
bool iso_extract_all(const std::filesystem::path &iso, const std::filesystem::path &dest_dir, std::string &err,
                     const IsoProgress &progress = {});
// Copy a single file (path inside the image, e.g. "PSP_GAME/SYSDIR/BOOT.BIN") to dest_file.
bool iso_extract_file(const std::filesystem::path &iso, const std::string &inner_path,
                      const std::filesystem::path &dest_file, std::string &err);

} // namespace rr
