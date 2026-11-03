#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

// We need a guest memory access that can do boundary checks.
// Here we only depend on two methods, and it is consistent with the GuestMemory:
//   std::span<uint8_t> span(gpa, len)   throw exception if out of boundary
//   uint64_t size()
// In order to make this header separately testable, use a template parameter to receive it.
//
// The three memory layouts of split virtqueue (virtio 1.x):
//   desc  : virtq_desc[num]                       16 bytes per item  
//   avail : {u16 flags; u16 idx; u16 ring[num]; } guest write, device read
//   used  : {u16 flags; u16 idx; struct{u32 id;u32 len;} ring[num]; } device write
struct VirtqDesc {               // the descriptor in guest memory, 16 bytes
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
static_assert(sizeof(VirtqDesc) == 16);

namespace vq {
    constexpr uint16_t F_NEXT  = 1;   // There is more in the chain
    constexpr uint16_t F_WRITE = 2;   // Device write, guest read (the section where data is loaded from disk)
}

// Traverse a chain to get a buffer: host pointer + length + whether the device is writable
struct Buffer {
    uint8_t* data;
    uint32_t len;
    bool     writable;
};

template <typename Mem>
class Virtqueue {
public:
    Virtqueue(Mem& mem, uint16_t num, uint64_t desc, uint64_t avail, uint64_t used)
        : mem_(mem), num_(num), desc_(desc), avail_(avail), used_(used) {
        if (num_ == 0 || (num_ & (num_ - 1)))     // has to be power of two (virtio spec)
            throw std::runtime_error("virtqueue: num must be power of two");
    }

    // guest submitted how many times through the available ring (monotonically increasing, will wrap around)
    uint16_t avail_idx() const { return load16(avail_ + 2); } // read the index of the available ring

    // There is no new chain to process
    bool has_pending() const { return last_avail_ != avail_idx(); } // check if there is a new chain to process

    // Get the next chain head descriptor number to process; if not, return false
    bool pop_head(uint16_t& head) {
        if (!has_pending()) return false;
        // The ring is circular, the index is modulo num_
        uint16_t slot = last_avail_ % num_;
        head = load16(avail_ + 4 + slot * 2);     // avail.ring[slot]
        if (head >= num_)                          // guest filled the chain head number arbitrarily
            throw std::runtime_error("virtqueue: head out of range");
        last_avail_++;
        return true;
    }

    // Traverse the entire descriptor chain from the chain head head, collect all segments. Do three checks:
    //   1) Each segment addr/len through mem_.span() → GPA boundary check
    //   2) The chain length does not exceed num_          → prevent overflow
    //   3) The number of steps exceeds num_ is considered to have a loop → prevent loop
    std::vector<Buffer> collect(uint16_t head) {
        std::vector<Buffer> out;
        uint16_t idx = head;
        for (uint32_t steps = 0; ; steps++) {
            if (steps >= num_)                     // Check 2 & 3: loop or overflow
                throw std::runtime_error("virtqueue: chain too long / loop");
            VirtqDesc d = load_desc(idx);
            std::span<uint8_t> s = mem_.span(d.addr, d.len);   // Check 1: throw exception if out of boundary
            out.push_back({s.data(), d.len, bool(d.flags & vq::F_WRITE)});
            if (!(d.flags & vq::F_NEXT)) break;    // No NEXT, chain ends
            if (d.next >= num_)                    // next points to outside the table
                throw std::runtime_error("virtqueue: next out of range");
            idx = d.next;
        }
        return out;
    }

    // After processing a chain, write the result back to the used ring:id = chain head,len = total bytes written to the writable section
    void push_used(uint16_t head, uint32_t written) {
        uint16_t idx = load16(used_ + 2);            // used.idx
        uint16_t slot = idx % num_;
        store32(used_ + 4 + slot * 8 + 0, head);     // used.ring[slot].id
        store32(used_ + 4 + slot * 8 + 4, written);  // used.ring[slot].len
        store16(used_ + 2, idx + 1);                 // Release: idx must be added last
    }

private:
    VirtqDesc load_desc(uint16_t i) {
        VirtqDesc d{};
        std::span<uint8_t> s = mem_.span(desc_ + uint64_t(i) * 16, 16);
        std::memcpy(&d, s.data(), 16);             // x86 little endian, directly copy
        return d;
    }
    uint16_t load16(uint64_t gpa) const { uint16_t v; std::memcpy(&v, mem_.span(gpa,2).data(),2); return v; }
    void store16(uint64_t gpa, uint16_t v){ std::memcpy(mem_.span(gpa,2).data(), &v, 2); }
    void store32(uint64_t gpa, uint32_t v){ std::memcpy(mem_.span(gpa,4).data(), &v, 4); }

    Mem& mem_;
    uint16_t num_; // number of descriptors in the queue
    uint64_t desc_, avail_, used_; // pointers to the guest memory of descriptor, available ring and used ring
    uint16_t last_avail_ = 0;      // We have processed to which position in the available ring
};