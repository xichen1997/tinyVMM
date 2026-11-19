#pragma once

#include <linux/kvm.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vector>

// One device occupies a section of MMIO address. 
// The offset is the address of the device minus the start address of the device.
// Unlike IoDevice, the entire access is given to the device (len = 1/2/4/8), not split into single bytes,
// because the virtio-mmio registers are 32 bits, so reading a register should be a read.
struct MmioDevice {
    virtual void read(uint64_t offset, uint8_t* data, uint32_t len) = 0;
    virtual void write(uint64_t offset, const uint8_t* data, uint32_t len) = 0;
    virtual ~MmioDevice() = default;
};

// The address to device table.
class MmioBus {
public:
    void add(uint64_t base, uint64_t len, MmioDevice* dev) {
        for (const Entry& e : entries_)
            if (base < e.base + e.len && e.base < base + len)
                throw std::runtime_error("MmioBus: address ranges overlap");
        entries_.push_back({base, len, dev});
    }

    // Handle one KVM_EXIT_MMIO.
    void handle(kvm_run* run) {
        uint64_t addr = run->mmio.phys_addr;
        uint32_t len = run->mmio.len;
        uint8_t* data = run->mmio.data;   // at most 8 bytes, filled by us when reading
        
        for (const Entry& e : entries_) {
            // The entire access must fall within the device range.
            if (addr >= e.base && addr + len <= e.base + e.len) {
                uint64_t offset = addr - e.base;
                if (run->mmio.is_write)
                    e.dev->write(offset, data, len);
                else
                    e.dev->read(offset, data, len);
                return;
            }
        }

        // No device at the address: read out all 1, discard write.
        if (!run->mmio.is_write)
            std::memset(data, 0xFF, len);
        if (logged_.insert(addr).second)
            std::fprintf(stderr, "[mmio] unhandled %s %#lx len %u\n",
                         run->mmio.is_write ? "write" : "read",
                         (unsigned long)addr, len);
    }

private:
    struct Entry {
        uint64_t base;
        uint64_t len;
        MmioDevice* dev;
    };

    std::vector<Entry> entries_;
    std::set<uint64_t> logged_;
};