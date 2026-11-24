#pragma once
#include "virtio/virtio.hpp"
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include "vmm/fd.hpp"

// virtio-blk request layout (virtio spec 5.2.6), one descriptor chain per request:
//   [header 16B, device-readable] [data segments ...] [status 1B, device-writable]
namespace vblk {
    constexpr uint32_t T_IN     = 0;    // read  : disk -> guest, data segments are device-writable
    constexpr uint32_t T_OUT    = 1;    // write : guest -> disk, data segments are device-readable
    constexpr uint32_t T_FLUSH  = 4;    // flush : no data segments
    constexpr uint8_t  S_OK     = 0;
    constexpr uint8_t  S_IOERR  = 1;
    constexpr uint8_t  S_UNSUPP = 2;
    constexpr uint64_t SECTOR   = 512;
}


//struct VirtioDevice {
    // virtual uint32_t device_id() = 0;                               // 1=net 2=blk 3=console
    // virtual uint64_t features() = 0;                                // at least F_VERSION_1
    // virtual int num_queues() = 0;
    // virtual void read_config(uint64_t, uint8_t* data, uint32_t len) { std::memset(data, 0, len); }
    // virtual bool process_queue(int q, Virtqueue<GuestMemory>& vq) = 0;  // process the queue
    // virtual void queue_disabled(int) = 0;
    // virtual void reset() = 0;                        // reset the device
    // virtual ~VirtioDevice() = default;
//};

class VirtioBlk : public VirtioDevice {
public:
    VirtioBlk(FD fd) : fd_(std::move(fd)) { // fd is the file descriptor for the block device
        struct stat st{};
        if (::fstat(fd_.get(), &st) < 0)
            throw std::system_error(errno, std::generic_category(), "virtio-blk: fstat");
        if (st.st_size % vblk::SECTOR != 0)
            throw std::runtime_error("virtio-blk: image size is not a multiple of 512");
        capacity_ = uint64_t(st.st_size) / vblk::SECTOR;
    }
    uint32_t device_id() override { return 2;} // 2 is the device id for the virtio-blk device
    uint64_t features() override { return vmmio::F_VERSION_1; }
    int num_queues() override { return 1;}

    // config space (spec 5.2.4): offset 0 = u64 capacity in 512-byte sectors, little endian.
    // The guest may read it in any width/offset (Linux does two 32-bit reads), so copy byte by byte.
    void read_config(uint64_t off, uint8_t* data, uint32_t len) override {
        uint8_t cfg[8];
        std::memcpy(cfg, &capacity_, sizeof(cfg));   // x86 is little endian, same as virtio
        for (uint32_t i = 0; i < len; i++)
            data[i] = (off + i < sizeof(cfg)) ? cfg[off + i] : 0;
    }

    // return true = put at least one used entry into the used ring, Transport needs to call signal_used
    bool process_queue(int q, Virtqueue<GuestMemory>& vq) override {
        if( q != 0) {
            std::cerr << "VirtioBlk: Invalid queue index: " << q << std::endl;
            return false;
        }
        return process_request(vq);   // false = nothing pending (a spurious kick), not an error
    }

    // blk keeps no pointer into the queue and no buffered data, nothing to clean up
    void queue_disabled(int) override {}
    void reset() override {}

private:
    bool process_request(Virtqueue<GuestMemory>& vq){
        bool any = false;
        uint16_t head;
        while (vq.pop_head(head)) {
            // read and write for the block device
            std::vector<Buffer> bufs = vq.collect(head);   // throws on bad GPA / loop -> transport needs_reset

            // chain must be: header (readable, >=16B) ... status (writable, >=1B)
            if (bufs.size() < 2 || bufs.front().writable || bufs.front().len < 16 ||
                !bufs.back().writable || bufs.back().len < 1)
                throw std::runtime_error("virtio-blk: malformed request chain");

            uint32_t type;
            uint64_t sector;
            std::memcpy(&type,   bufs.front().data + 0, 4);   // memcpy: guest memory may be unaligned
            std::memcpy(&sector, bufs.front().data + 8, 8);   // offset 4 is reserved

            uint8_t  status  = vblk::S_OK;
            uint32_t written = 0;                             // bytes the device wrote into guest memory
            size_t   first = 1, last = bufs.size() - 1;       // data segments are bufs[first, last)

            switch (type) {
            case vblk::T_IN:
            case vblk::T_OUT: {
                const bool is_read = (type == vblk::T_IN);
                uint64_t total = 0;
                for (size_t i = first; i < last; i++) {
                    if (bufs[i].writable != is_read) { status = vblk::S_IOERR; break; } // wrong direction
                    total += bufs[i].len;
                }
                // bounds check, written so that a huge guest-chosen sector can't overflow
                if (status == vblk::S_OK &&
                    (sector > capacity_ || total > (capacity_ - sector) * vblk::SECTOR))
                    status = vblk::S_IOERR;
                if (status != vblk::S_OK) break;

                off_t off = off_t(sector * vblk::SECTOR);
                for (size_t i = first; i < last; i++) {
                    bool ok = is_read ? pread_full (fd_.get(), bufs[i].data, bufs[i].len, off)
                                      : pwrite_full(fd_.get(), bufs[i].data, bufs[i].len, off);
                    if (!ok) { status = vblk::S_IOERR; break; }
                    off += bufs[i].len;
                    if (is_read) written += bufs[i].len;
                }
                break;
            }
            case vblk::T_FLUSH:
                if (::fdatasync(fd_.get()) < 0) status = vblk::S_IOERR;
                break;
            default:
                status = vblk::S_UNSUPP;
                break;
            }

            bufs.back().data[0] = status;   // status is the first byte of the last segment
            written += 1;
            vq.push_used(head, written);    // after this the chain belongs to the guest again
            any = true;
        }
        return any;
    }

    // pread/pwrite may transfer fewer bytes than asked, or fail with EINTR (we have a SIGINT handler)
    static bool pread_full(int fd, uint8_t* p, size_t n, off_t off) {
        while (n > 0) {
            ssize_t r = ::pread(fd, p, n, off);
            if (r < 0) { if (errno == EINTR) continue; return false; }
            if (r == 0) return false;      // EOF inside the disk: bounds check above should prevent this
            p += r; n -= size_t(r); off += r;
        }
        return true;
    }
    static bool pwrite_full(int fd, const uint8_t* p, size_t n, off_t off) {
        while (n > 0) {
            ssize_t r = ::pwrite(fd, p, n, off);
            if (r < 0) { if (errno == EINTR) continue; return false; }
            p += r; n -= size_t(r); off += r;
        }
        return true;
    }

    FD fd_;
    uint64_t capacity_ = 0; // how many sectors in the block device.
};