// clang++ -std=c++20 -g -fsanitize=address,undefined gdt_test.cpp -o gdt_test && ./gdt_test
#include "vmm/gdt.hpp"

#include <cstdio>
#include <cstdlib>

static int failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

// test 1: use known standard descriptor to check decode is correct
// 0x00af9a000000ffff is a common 64-bit kernel code segment
static void test_decode_code64() {
    SegmentDesc d = decode(0x00af9a000000ffffull);
    CHECK(d.base == 0);
    CHECK(d.limit == 0xFFFFF);
    CHECK(d.type == 0xA);      // 可执行 + 可读
    CHECK(d.s == 1);
    CHECK(d.dpl == 0);
    CHECK(d.present == 1);
    CHECK(d.l == 1);           // 64 位代码段
    CHECK(d.db == 0);          // l = 1 时 db 必须为 0
    CHECK(d.g == 1);
}

// test 2: use known standard descriptor to check decode is correct
// 0x00cf92000000ffff is a common 64-bit kernel data segment
static void test_decode_data() {
    SegmentDesc d = decode(0x00cf92000000ffffull);
    CHECK(d.type == 0x2);      // 可读写数据
    CHECK(d.l == 0);
    CHECK(d.db == 1);
    CHECK(d.g == 1);
    CHECK(d.present == 1);
}

// test 3: use known standard descriptor to check encode is correct
static void test_encode_code64() {
    SegmentDesc d;
    d.limit = 0xFFFFF;
    d.type = 0xA;
    d.s = 1;
    d.present = 1;
    d.l = 1;
    d.g = 1;
    CHECK(encode(d) == 0x00af9a000000ffffull);
}

// test 4: roundtrip
static void test_roundtrip() {
    const uint64_t samples[] = {
        0x00af9a000000ffffull,
        0x00cf92000000ffffull,
        0x12345678abcdef01ull & 0xFFFFFFFFFFFFFFFFull,
        0xff4ff2ff12345678ull,
    };
    for (uint64_t v : samples)
        CHECK(encode(decode(v)) == v);
}

// test 5: convert to kvm_segment, when g = 1, limit is calculated as 0xFFFFFFFF
static void test_to_kvm_segment() {
    kvm_segment s = to_kvm_segment(decode(0x00af9a000000ffffull), 0x08);
    CHECK(s.selector == 0x08);
    CHECK(s.base == 0);
    CHECK(s.limit == 0xFFFFFFFF);   // 0xFFFFF 个 4 KB 页 = 4 GB - 1
    CHECK(s.l == 1);
    CHECK(s.type == 0xA);

    SegmentDesc small;              // g = 0：limit 就是字节数，不换算
    small.limit = 0x1234;
    kvm_segment s2 = to_kvm_segment(small, 0);
    CHECK(s2.limit == 0x1234);
}

int main() {
    test_decode_code64();
    test_decode_data();
    test_encode_code64();
    test_roundtrip();
    test_to_kvm_segment();

    if (failures == 0) {
        std::puts("all tests passed");
        return 0;
    }
    std::printf("%d check(s) failed\n", failures);
    return 1;
}