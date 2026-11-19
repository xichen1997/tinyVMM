#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

struct BzImage {
    std::vector<uint8_t> setup_header;   
    std::vector<uint8_t> kernel;        

    uint16_t version = 0;                
    uint8_t  setup_sects = 0;           
    uint32_t init_size = 0;             
    uint32_t cmdline_size = 0;    
};

namespace bz {
constexpr size_t SETUP_SECTS  = 0x1f1;
constexpr size_t BOOT_FLAG    = 0x1fe;
constexpr size_t JUMP         = 0x200;
constexpr size_t HEADER_MAGIC = 0x202;
constexpr size_t VERSION      = 0x206;
constexpr size_t XLOADFLAGS   = 0x236;
constexpr size_t CMDLINE_SIZE = 0x238;
constexpr size_t INIT_SIZE    = 0x260;

constexpr uint32_t HDRS          = 0x53726448;  
constexpr uint16_t MIN_VERSION   = 0x020c;      
constexpr uint16_t XLF_KERNEL_64 = 1 << 0;      
}  // namespace bz

template <typename T>
T read_le(const std::vector<uint8_t>& buf, size_t off) {
    if (off > buf.size() || sizeof(T) > buf.size() - off)
        throw std::runtime_error("bzImage: read past end of file");
    T v;
    std::memcpy(&v, buf.data() + off, sizeof(T));  
    return v;
}

inline BzImage parse_bzimage(const std::vector<uint8_t>& file) {
    using namespace bz;

    if (read_le<uint16_t>(file, BOOT_FLAG) != 0xAA55)
        throw std::runtime_error("bzImage: boot_flag is not 0xAA55");
    if (read_le<uint32_t>(file, HEADER_MAGIC) != HDRS)
        throw std::runtime_error("bzImage: no \"HdrS\" magic, not a bzImage");

    BzImage img;

    img.version = read_le<uint16_t>(file, VERSION);
    if (img.version < MIN_VERSION)
        throw std::runtime_error("bzImage: boot protocol too old (need 2.12+)");

    uint16_t xloadflags = read_le<uint16_t>(file, XLOADFLAGS);
    if (!(xloadflags & XLF_KERNEL_64))
        throw std::runtime_error("bzImage: no 64-bit entry point");

    img.init_size    = read_le<uint32_t>(file, INIT_SIZE);
    img.cmdline_size = read_le<uint32_t>(file, CMDLINE_SIZE);

    size_t header_end = 0x202 + read_le<uint8_t>(file, JUMP + 1);
    if (header_end > file.size())
        throw std::runtime_error("bzImage: setup header runs past end of file");
    img.setup_header.assign(file.begin() + SETUP_SECTS, file.begin() + header_end);

    img.setup_sects = read_le<uint8_t>(file, SETUP_SECTS);
    if (img.setup_sects == 0)
        img.setup_sects = 4;   
    size_t kernel_off = (img.setup_sects + 1) * 512;
    if (kernel_off >= file.size())
        throw std::runtime_error("bzImage: no protected-mode kernel after setup");
    img.kernel.assign(file.begin() + kernel_off, file.end());

    return img;
}

inline BzImage load_bzimage(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> file{std::istreambuf_iterator<char>(f), {}};
    return parse_bzimage(file);
}