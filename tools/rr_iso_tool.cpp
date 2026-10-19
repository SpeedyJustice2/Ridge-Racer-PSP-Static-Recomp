// Small command-line helper used by build.ps1 so the build can take an .iso directly.
//   rr_iso_tool <image.iso> --file <path inside image> <destination file>
//   rr_iso_tool <image.iso> --all <destination folder>
// Exit code 0 = ok; 1 = failed (reason on stderr).
#include "rr_iso.hpp"

#include <cstdio>
#include <string>

int main(int argc, char **argv) {
    if (argc >= 5 && std::string(argv[2]) == "--file") {
        std::string err;
        if (!rr::iso_extract_file(argv[1], argv[3], argv[4], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        return 0;
    }
    if (argc >= 4 && std::string(argv[2]) == "--all") {
        std::string err;
        if (!rr::iso_extract_all(argv[1], argv[3], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        return 0;
    }
    std::fprintf(stderr, "usage: rr_iso_tool <image.iso> --file <inner path> <dest file> | --all <dest folder>\n");
    return 1;
}
