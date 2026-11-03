#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>



#include "mmiobus.hpp"
#include "guest_memory.hpp"
#include "virtqueue.hpp"


// virtio-mmio register offsets (only the ones we use, version 2)
namespace vmmio {
    constexpr uint64_t MAGIC            = 0x000;
    constexpr uint64_t VERSION          = 0x004;
    constexpr uint64_t DEVICE_ID        = 0x008;
    constexpr uint64_t VENDOR_ID        = 0x00c;
    constexpr uint64_t DEVICE_FEATURES  = 0x010;
    constexpr uint64_t DEVICE_FEAT_SEL  = 0x014;
    constexpr uint64_t DRIVER_FEATURES  = 0x020;
    constexpr uint64_t DRIVER_FEAT_SEL  = 0x024;
    constexpr uint64_t QUEUE_SEL        = 0x030;
    constexpr uint64_t QUEUE_NUM_MAX    = 0x034;
    constexpr uint64_t QUEUE_NUM        = 0x038;
    constexpr uint64_t QUEUE_READY      = 0x044;
    constexpr uint64_t QUEUE_NOTIFY     = 0x050;
    constexpr uint64_t INTERRUPT_STATUS = 0x060;
    constexpr uint64_t INTERRUPT_ACK    = 0x064;
    constexpr uint64_t STATUS           = 0x070;
    constexpr uint64_t QUEUE_DESC_LOW   = 0x080;
    constexpr uint64_t QUEUE_DESC_HIGH  = 0x084;
    constexpr uint64_t QUEUE_AVAIL_LOW  = 0x090;
    constexpr uint64_t QUEUE_AVAIL_HIGH = 0x094;
    constexpr uint64_t QUEUE_USED_LOW   = 0x0a0;
    constexpr uint64_t QUEUE_USED_HIGH  = 0x0a4;
    constexpr uint64_t CONFIG_GEN       = 0x0fc;
    constexpr uint64_t CONFIG           = 0x100;

    constexpr uint32_t MAGIC_VALUE = 0x74726976;   // "virt"
    constexpr uint64_t F_VERSION_1 = 1ull << 32;   // version 2 device must provide
    constexpr int MAX_QUEUES = 2;                  // console/net 2 queues,blk 1 queue,enough for now
    constexpr uint16_t QUEUE_SIZE = 256;
}

// specific devices just answer these questions.
struct VirtioDevice {
    virtual uint32_t device_id() = 0;                               // 1=net 2=blk 3=console
    virtual uint64_t features() = 0;                                // at least F_VERSION_1
    virtual int num_queues() = 0;
    virtual void read_config(uint64_t, uint8_t* data, uint32_t len) { std::memset(data, 0, len); }
    virtual void process_queue(int q) = 0;                          // guest wrote QueueNotify
    virtual ~VirtioDevice() = default;
};

class VirtioMmioTransport : public MmioDevice {
public:
    // raise_irq: raise the IRQ line of this device to level(trigger level),like the update_irq in serial port
    VirtioMmioTransport(VirtioDevice& dev, std::function<void(bool)> raise_irq, GuestMemory& mem)
        : dev_(dev), raise_irq_(std::move(raise_irq)), mem_(mem) {}

    struct Queue {
        uint16_t num = 0;
        bool     ready = false;
        uint64_t desc = 0, avail = 0, used = 0;
        std::optional<Virtqueue<GuestMemory>> vq;
    };

    Queue& queue(int i) { return q_[i]; }

    // after the device processed the request and updated the used ring,call:set the bit and raise the IRQ
    void signal_used() {
        interrupt_status_ |= 0x1;       // bit0 = queue has been updated
        raise_irq_(true);
    }

    void read(uint64_t off, uint8_t* data, uint32_t len) override {
        uint32_t v = read_reg(off, len, data);
        if (off < vmmio::CONFIG)        // control register:copy the 32-bit value back
            std::memcpy(data, &v, 4);
        // The configuration space is filled directly in read_reg
    }

    void write(uint64_t off, const uint8_t* data, uint32_t len) override {
        if (off >= vmmio::CONFIG) return;         // config space is read-only,ignore write
        if (len != 4 || off % 4) return;          // control register only accepts 4-byte aligned access
        uint32_t v;
        std::memcpy(&v, data, 4);
        write_reg(off, v);
    }

private:
    void reset() {
        status_ = 0;
        interrupt_status_ = 0;
        queue_sel_ = dev_feat_sel_ = drv_feat_sel_ = 0;
        driver_features_ = 0;
        for (auto& q : q_) {
            q.ready = false;
            q.vq.reset();
            q.num = 0;
            q.desc = 0;
            q.avail = 0;
            q.used = 0;
        }
        raise_irq_(false);
    }

    void set_queue_ready(bool ready) {
        if (ready) {
            // check if the queue number is valid
            if(cur().num == 0){
                // print log 
                std::fprintf(stderr, "[virtio] queue number is 0\n");
                return;
            }
            if(cur().num > vmmio::QUEUE_SIZE || (cur().num & (cur().num - 1))){
                // print log 
                std::fprintf(stderr, "[virtio] queue number is not a power of two: %u\n", cur().num);
                return;
            }
            cur().vq.emplace(mem_, cur().num, cur().desc, cur().avail, cur().used);
        } else {
            cur().vq.reset();
        }
        cur().ready = ready;
    }

    uint32_t read_reg(uint64_t off, uint32_t len, uint8_t* data) {
        if (off >= vmmio::CONFIG) {               // config space:any width, give to the device
            dev_.read_config(off - vmmio::CONFIG, data, len);
            return 0;
        }
        switch (off) {
        case vmmio::MAGIC:       return vmmio::MAGIC_VALUE;
        case vmmio::VERSION:     return 2;
        case vmmio::DEVICE_ID:   return dev_.device_id();
        case vmmio::VENDOR_ID:   return 0;
        case vmmio::DEVICE_FEATURES:
            return dev_feat_sel_ ? uint32_t(dev_.features() >> 32)
                                 : uint32_t(dev_.features());
        case vmmio::QUEUE_NUM_MAX:
            return (int(queue_sel_) < dev_.num_queues()) ? vmmio::QUEUE_SIZE : 0;
        case vmmio::QUEUE_READY:
            return cur().ready ? 1 : 0;
        case vmmio::INTERRUPT_STATUS: return interrupt_status_;
        case vmmio::STATUS:          return status_;
        case vmmio::CONFIG_GEN:      return 0;
        default:                     return 0;    // other registers read 0
        }
    }

    void write_reg(uint64_t off, uint32_t v) {
        switch (off) {
        case vmmio::DEVICE_FEAT_SEL: dev_feat_sel_ = v; break;
        case vmmio::DRIVER_FEAT_SEL: drv_feat_sel_ = v; break;
        case vmmio::DRIVER_FEATURES:
            if (drv_feat_sel_) driver_features_ = (driver_features_ & 0xffffffff) | (uint64_t(v) << 32);
            else               driver_features_ = (driver_features_ & ~0xffffffffull) | v;
            break;
        case vmmio::QUEUE_SEL:
            if (v < vmmio::MAX_QUEUES) queue_sel_ = v;
            break;
        case vmmio::QUEUE_NUM:   {
            if(v == 0 || v > vmmio::QUEUE_SIZE || (v & (v - 1))){
                // print log 
                std::fprintf(stderr, "[virtio] queue number is not a power of two or is 0: %u\n", v);
                break;
            }
            cur().num = v;
            break;
        }
        case vmmio::QUEUE_READY: {
            set_queue_ready(v == 1);
            break;
        }
        case vmmio::QUEUE_DESC_LOW:   set_lo(cur().desc, v);  break;
        case vmmio::QUEUE_DESC_HIGH:  set_hi(cur().desc, v);  break;
        case vmmio::QUEUE_AVAIL_LOW:  set_lo(cur().avail, v); break;
        case vmmio::QUEUE_AVAIL_HIGH: set_hi(cur().avail, v); break;
        case vmmio::QUEUE_USED_LOW:   set_lo(cur().used, v);  break;
        case vmmio::QUEUE_USED_HIGH:  set_hi(cur().used, v);  break;
        case vmmio::QUEUE_NOTIFY:
            if (v < vmmio::MAX_QUEUES) dev_.process_queue(v);
            break;
        case vmmio::INTERRUPT_ACK:
            interrupt_status_ &= ~v;
            if (interrupt_status_ == 0) raise_irq_(false);    // if cleared,lower the IRQ
            break;
        case vmmio::STATUS:
            if (v == 0) reset();
            else        status_ = v;
            std::fprintf(stderr, "[virtio] status = %#x\n", v);
            break;
        default: break;
        }
    }

    Queue& cur() { return q_[queue_sel_]; }
    static void set_lo(uint64_t& a, uint32_t v) { a = (a & ~0xffffffffull) | v; }
    static void set_hi(uint64_t& a, uint32_t v) { a = (a & 0xffffffff) | (uint64_t(v) << 32); }

    VirtioDevice& dev_;
    std::function<void(bool)> raise_irq_;
    GuestMemory& mem_;

    uint32_t status_ = 0;
    uint32_t interrupt_status_ = 0;
    uint32_t queue_sel_ = 0;
    uint32_t dev_feat_sel_ = 0, drv_feat_sel_ = 0;
    uint64_t driver_features_ = 0;
    Queue q_[vmmio::MAX_QUEUES];
};