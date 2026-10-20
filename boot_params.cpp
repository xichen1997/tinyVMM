#include <cstdio>
#include <cstdlib>
#include "boot_params.hpp"

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n",             \
                         __FILE__, __LINE__, #cond);                      \
            std::exit(1);                                                 \
        }                                                                 \
    } while (0)

template <typename T>
T get_le(const std::vector<uint8_t>& buf, size_t off) {
    T v;
    std::memcpy(&v, buf.data() + off, sizeof(T));
    return v;
}

// 一个只填了必要字段的 BzImage，不用真的解析文件
static BzImage fake_image() {
    BzImage img;
    img.setup_header.assign(0x26c - 0x1f1, 0);
    img.setup_header[0] = 39;                        // setup_sects
    img.setup_header[0x206 - 0x1f1] = 0x0f;          // version 低字节
    img.setup_header[0x207 - 0x1f1] = 0x02;          // version 高字节
    img.cmdline_size = 2047;
    return img;
}

int main() {
    constexpr uint64_t MEM = 256ull << 20;           // 256 MiB
    BzImage img = fake_image();
    auto p = build_boot_params(img, MEM, "console=ttyS0", 0x20000, 0x8000000, 0x100000);

    CHECK(p.size() == 4096);

    // setup header 原样到了 0x1f1，而且没被覆盖掉
    CHECK(p[0x1f1] == 39);
    CHECK(get_le<uint16_t>(p, 0x206) == 0x020f);

    // 引导程序填的字段
    CHECK(p[0x210] == 0xFF);
    CHECK(get_le<uint16_t>(p, 0x1fa) == 0xFFFF);
    CHECK(get_le<uint32_t>(p, 0x228) == 0x20000);
    CHECK(get_le<uint32_t>(p, 0x218) == 0x8000000);
    CHECK(get_le<uint32_t>(p, 0x21c) == 0x100000);

    // e820：3 项，首尾相接，覆盖 0 到内存末尾
    CHECK(p[0x1e8] == 3);
    E820Entry e[3];
    std::memcpy(e, p.data() + 0x2d0, sizeof(e));
    CHECK(e[0].addr == 0        && e[0].size == 0x9fc00 && e[0].type == 1);
    CHECK(e[1].addr == 0x9fc00  && e[1].type == 2);
    CHECK(e[1].addr + e[1].size == 0x100000);
    CHECK(e[2].addr == 0x100000 && e[2].type == 1);
    CHECK(e[2].addr + e[2].size == MEM);

    // 其他地方都是 0
    CHECK(p[0] == 0 && p[0x1e7] == 0 && p[0xfff] == 0);

    // 出错情况
    bool threw = false;
    try { build_boot_params(img, 0x100000, "", 0); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { build_boot_params(img, MEM, std::string(2048, 'x'), 0x20000); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    std::puts("all tests passed");
}