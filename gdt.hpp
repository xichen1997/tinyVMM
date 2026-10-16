#pragma once

#include <linux/kvm.h>
#include <cstdint>

// 一个段描述符的各个字段（拆开后的样子）
struct SegmentDesc {
    uint32_t base = 0;     // 段起始地址（16 位）
    uint32_t limit = 0;    // 段界限（20 位，单位由 g 决定）
    uint8_t type = 0;      // 4 位：代码/数据、可读/可写、是否访问过
    uint8_t s = 0;         // 1 = 代码或数据段，0 = 系统段（如 TSS）
    uint8_t dpl = 0;       // 特权级 0~3 
    uint8_t present = 0;   // 描述符是否有效
    uint8_t avl = 0;       // 留给软件自用
    uint8_t l = 0;         // 1 = 64 位代码段 
    uint8_t db = 0;        // 1 = 32 位段
    uint8_t g = 0;         // 1 = limit 以 4 KB 为单位
};

// 字段 → 8 字节描述符
//
// 位布局（Intel SDM 规定，字段被拆散在不同位置，这是历史原因）：
//   0-15  limit[0:15]      16-39 base[0:23]
//   40-43 type             44    s
//   45-46 dpl              47    present
//   48-51 limit[16:19]     52    avl
//   53    l                54    db
//   55    g                56-63 base[24:31]
inline uint64_t encode(const SegmentDesc& d) {
    uint64_t v = 0;
    v |= uint64_t(d.limit & 0xFFFF);                 // lower 16 bits of limit
    v |= uint64_t(d.base & 0xFFFFFF) << 16;          // lower 24 bits of base
    v |= uint64_t(d.type & 0xF) << 40;               // lower 4 bits of type
    v |= uint64_t(d.s & 1) << 44;                    // s
    v |= uint64_t(d.dpl & 3) << 45;                  // dpl
    v |= uint64_t(d.present & 1) << 47;              // present
    v |= uint64_t((d.limit >> 16) & 0xF) << 48;      // upper 4 bits of limit
    v |= uint64_t(d.avl & 1) << 52;                  // avl
    v |= uint64_t(d.l & 1) << 53;                    // l
    v |= uint64_t(d.db & 1) << 54;                   // db
    v |= uint64_t(d.g & 1) << 55;                    // g
    v |= uint64_t((d.base >> 24) & 0xFF) << 56;      // upper 8 bits of base
    return v;
}

// 8 字节描述符 → 字段（encode 的逆操作）
inline SegmentDesc decode(uint64_t v) {
    SegmentDesc d;
    d.limit   = uint32_t(v & 0xFFFF) | uint32_t((v >> 48) & 0xF) << 16;
    d.base    = uint32_t((v >> 16) & 0xFFFFFF) | uint32_t((v >> 56) & 0xFF) << 24;
    d.type    = (v >> 40) & 0xF;
    d.s       = (v >> 44) & 1;
    d.dpl     = (v >> 45) & 3;
    d.present = (v >> 47) & 1;
    d.avl     = (v >> 52) & 1;
    d.l       = (v >> 53) & 1;
    d.db      = (v >> 54) & 1;
    d.g       = (v >> 55) & 1;
    return d;
}

// 字段 → KVM 的段寄存器（也就是 CPU 从 GDT 读出后缓存的样子）
// selector 是这个段在 GDT 里的选择子，例如索引 1 → 0x08
inline kvm_segment to_kvm_segment(const SegmentDesc& d, uint16_t selector) {
    kvm_segment s{};
    s.base = d.base;
    // kvm_segment.limit 以字节为单位：g = 1 时要换算成 (limit << 12) | 0xFFF
    s.limit = d.g ? (d.limit << 12) | 0xFFF : d.limit;
    s.selector = selector;
    s.type = d.type;
    s.present = d.present;
    s.dpl = d.dpl;
    s.db = d.db;
    s.s = d.s;
    s.l = d.l;
    s.g = d.g;
    s.avl = d.avl;
    return s;
}