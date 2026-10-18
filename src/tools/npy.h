#pragma once
// Minimal .npy writer (numpy format 1.0, little-endian, C order) for the fitting tools.
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pf {

// Fixed 128-byte header, so a streamed file's header can be rewritten in place once N is known.
inline std::string npyHeader(const char* descr, size_t n, int width) {
    std::string d = std::string("{'descr': '") + descr + "', 'fortran_order': False, 'shape': (" + std::to_string(n) +
                    (width > 1 ? ", " + std::to_string(width) + ")" : ",)") + ", }";
    d.resize(128 - 10 - 1, ' ');
    d += '\n';
    return std::string("\x93NUMPY\x01\x00", 8) + char(d.size() & 0xff) + char(d.size() >> 8) + d;
}

// v as an (n / width) x width array (1-D when width == 1).
template <class T>
void writeNpy(const std::filesystem::path& path, const char* descr, const std::vector<T>& v, int width = 1) {
    std::ofstream f(path, std::ios::binary);
    f << npyHeader(descr, v.size() / width, width);
    f.write(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
}

}  // namespace pf
