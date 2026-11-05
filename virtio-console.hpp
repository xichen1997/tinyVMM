#pragma once
#include "virtio.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

// virtio-console：queue 0 = rx, queue 1 = tx
class VirtioConsole : public VirtioDevice {
public:
    uint32_t device_id() override { return 3; }
    uint64_t features() override { return vmmio::F_VERSION_1; }   // version 2 device must provide
    int num_queues() override { return 2; }

    // return true = put a used entry into the used ring, Transport needs to call signal_used
    bool process_queue(int q, Virtqueue<GuestMemory>& vq) override {
        if (q == 0) {               // guest has filled the rx empty buffer
            rx_ = &vq;
            return fill_rx();       // the input that has been buffered can now be sent in
        }
        if (q == 1) return drain_tx(vq);
        return false;
    }

    // stdin thread calls, must hold dev_lock before calling
    bool push_input(const uint8_t* buf, size_t n) {
        pending_.insert(pending_.end(), buf, buf + n);
        return fill_rx();
    }

    // Transport::reset() is called, the queue has been destroyed
    void reset() override {
        rx_ = nullptr;
        pending_.clear();
    }

    void queue_disabled(int q) override {
        if (q == 0) rx_ = nullptr;
    }

private:
    bool drain_tx(Virtqueue<GuestMemory>& vq) {
        bool used = false;
        uint16_t head;
        while (vq.pop_head(head)) {
            for (const Buffer& b : vq.collect(head))
                if (!b.writable)                       // tx data segment is read-only for the device
                    std::fwrite(b.data, 1, b.len, stdout);
            vq.push_used(head, 0);                     // the device didn't write to the guest, len = 0
            used = true;
        }
        std::fflush(stdout);
        return used;
    }

    bool fill_rx() {
        if (!rx_) return false;                        // guest hasn't built the rx queue yet
        bool used = false;
        uint16_t head;
        while (!pending_.empty() && rx_->pop_head(head)) {
            uint32_t written = 0;
            for (const Buffer& b : rx_->collect(head)) {
                if (!b.writable) continue;
                size_t n = std::min<size_t>(b.len, pending_.size());
                std::memcpy(b.data, pending_.data(), n);
                pending_.erase(pending_.begin(), pending_.begin() + n);
                written += uint32_t(n);
                if (pending_.empty()) break;
            }
            rx_->push_used(head, written);             // len = the number of bytes actually written
            used = true;
        }
        return used;
    }

    Virtqueue<GuestMemory>* rx_ = nullptr;
    std::vector<uint8_t> pending_;                     // the input that has been buffered but not sent to the guest
};