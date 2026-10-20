#include <cstdio>
#include <cstdlib>
#include "bzimage.hpp"

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n",             \
                         __FILE__, __LINE__, #cond);                      \
            std::exit(1);                                                 \
        }                                                                 \
    } while (0)

template <typename T>
void put(std::vector<uint8_t>& buf, size_t off, T v) {
    std::memcpy(buf.data() + off, &v, sizeof(T));
}

// forge a minimal valid image: parameters from the file output of Debian 6.12 kernel
static std::vector<uint8_t> fake_image() {
    std::vector<uint8_t> f(0x5000 + 64, 0);
    put<uint8_t >(f, 0x1f1, 39);            // setup_sects
    put<uint16_t>(f, 0x1fe, 0xAA55);        // boot_flag
    put<uint8_t >(f, 0x200, 0xEB);          // jmp
    put<uint8_t >(f, 0x201, 0x6a);          // header offset: 0x202 + 0x6a = 0x26c
    put<uint32_t>(f, 0x202, 0x53726448);    // "HdrS"
    put<uint16_t>(f, 0x206, 0x020f);        // 2.15
    put<uint16_t>(f, 0x236, 0x0001);        // XLF_KERNEL_64
    put<uint32_t>(f, 0x238, 2047);          // cmdline_size
    put<uint32_t>(f, 0x260, 0x37ef000);     // init_size
    f[0x5000] = 0xAB;                       // kernel first byte
    return f;
}

template <typename F>
static bool throws(F fn) {
    try { fn(); } catch (const std::runtime_error&) { return true; }
    return false;
}

int main(int argc, char** argv) {
    if (argc > 1) {
        BzImage img = load_bzimage(argv[1]);
        std::printf("protocol      %u.%02u\n", img.version >> 8, img.version & 0xff);
        std::printf("setup_sects   %u\n", img.setup_sects);
        std::printf("kernel offset %#x\n", (img.setup_sects + 1) * 512);
        std::printf("kernel size   %zu bytes\n", img.kernel.size());
        std::printf("setup header  %zu bytes\n", img.setup_header.size());
        std::printf("init_size     %#x\n", img.init_size);
        std::printf("cmdline_size  %u\n", img.cmdline_size);
        return 0;
    }

    BzImage img = parse_bzimage(fake_image());
    CHECK(img.version == 0x020f);
    CHECK(img.setup_sects == 39);
    CHECK(img.init_size == 0x37ef000);
    CHECK(img.cmdline_size == 2047);
    CHECK(img.setup_header.size() == 0x26c - 0x1f1);
    CHECK(img.setup_header[0] == 39);              // header 第一个字节就是 setup_sects
    CHECK(img.kernel.size() == 64);
    CHECK(img.kernel[0] == 0xAB);

    // setup_sects = 0 should be treated as 4
    auto f = fake_image();
    f[0x1f1] = 0;
    CHECK(parse_bzimage(f).kernel.size() == f.size() - 5 * 512);

    // various bad images should throw an error
    f = fake_image(); f[0x202] = 'X';              CHECK(throws([&] { parse_bzimage(f); }));
    f = fake_image(); f[0x1fe] = 0;                CHECK(throws([&] { parse_bzimage(f); }));
    f = fake_image(); put<uint16_t>(f, 0x206, 0x0204);
                                                   CHECK(throws([&] { parse_bzimage(f); }));
    f = fake_image(); put<uint16_t>(f, 0x236, 0);  CHECK(throws([&] { parse_bzimage(f); }));
    f = fake_image(); f.resize(0x100);             CHECK(throws([&] { parse_bzimage(f); }));
    f = fake_image(); f.resize(0x5000);            CHECK(throws([&] { parse_bzimage(f); }));

    std::puts("all tests passed");
    return 0;
}