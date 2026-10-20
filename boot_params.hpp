#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "bzimage.hpp"

// boot_params（"zero page"）：4096 字节，VMM 填好后放进 guest 内存，
// 把它的 GPA 放进 rsi 交给内核。字段偏移见 Documentation/arch/x86/zero-page.rst。
namespace bp {
constexpr size_t SIZE         = 4096;

constexpr size_t E820_ENTRIES = 0x1e8;   // 1 字节：e820 表有几项
constexpr size_t HDR          = 0x1f1;   // setup header 从这里开始，和 bzImage 文件里的偏移相同
constexpr size_t VID_MODE     = 0x1fa;   // 2 字节
constexpr size_t TYPE_OF_LOADER = 0x210; // 1 字节
constexpr size_t RAMDISK_IMAGE  = 0x218; // 4 字节
constexpr size_t RAMDISK_SIZE   = 0x21c; // 4 字节
constexpr size_t CMD_LINE_PTR   = 0x228; // 4 字节
constexpr size_t E820_TABLE   = 0x2d0;   // 每项 20 字节，最多 128 项

constexpr uint32_t E820_RAM      = 1;
constexpr uint32_t E820_RESERVED = 2;
}  // namespace bp

// e820 表的一项，内存里紧凑排列，没有对齐填充
struct __attribute__((packed)) E820Entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
};
static_assert(sizeof(E820Entry) == 20);

template <typename T>
void put_le(std::vector<uint8_t>& buf, size_t off, T v) {
    std::memcpy(buf.data() + off, &v, sizeof(T));   // x86 是小端，直接拷
}

// 生成 boot_params 的 4096 字节。
//   mem_size     guest 内存总大小
//   cmdline_gpa  命令行字符串在 guest 里的地址（由调用者写进 guest 内存）
//   initrd_gpa / initrd_size  没有 initrd 就传 0
inline std::vector<uint8_t> build_boot_params(const BzImage& img,
                                              uint64_t mem_size,
                                              const std::string& cmdline,
                                              uint32_t cmdline_gpa,
                                              uint32_t initrd_gpa = 0,
                                              uint32_t initrd_size = 0) {
    using namespace bp;

    if (mem_size <= 0x100000)
        throw std::runtime_error("boot_params: guest memory must be larger than 1 MiB");
    if (cmdline.size() > img.cmdline_size)
        throw std::runtime_error("boot_params: command line longer than kernel allows");

    std::vector<uint8_t> p(SIZE, 0);   // 协议要求：先全部清零

    // 1. setup header：从 bzImage 原样拷过来
    std::memcpy(p.data() + HDR, img.setup_header.data(), img.setup_header.size());

    // 2. 引导程序要填的字段
    put_le<uint8_t >(p, TYPE_OF_LOADER, 0xFF);     // 没有分配 ID 的引导程序填 0xFF
    put_le<uint16_t>(p, VID_MODE, 0xFFFF);         // "normal"，不切换显示模式
    put_le<uint32_t>(p, CMD_LINE_PTR, cmdline_gpa);
    put_le<uint32_t>(p, RAMDISK_IMAGE, initrd_gpa);
    put_le<uint32_t>(p, RAMDISK_SIZE, initrd_size);

    // 3. e820 内存表
    const E820Entry table[] = {
        {0x0,      0x9fc00,             E820_RAM},       // 低端 639 KiB
        {0x9fc00,  0x100000 - 0x9fc00,  E820_RESERVED},  // 到 1 MiB 为止保留
        {0x100000, mem_size - 0x100000, E820_RAM},       // 1 MiB 以上到内存末尾
    };
    std::memcpy(p.data() + E820_TABLE, table, sizeof(table));
    put_le<uint8_t>(p, E820_ENTRIES, std::size(table));

    return p;
}