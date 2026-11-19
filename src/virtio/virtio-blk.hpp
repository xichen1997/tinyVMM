#pragma once
#include "virtio/virtio.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

class VirtioBlk : public VirtioDevice {
public:
    uint32_t device_id() override { return 2;}
    uint64_t features() override { return vmmio::F_VERSION_1; }
    int num_queues() override { return 1;}
    bool process_queue(int q, Virtqueue<GuestMemory>& vq) override {
        
    }
private:
    Virtqueue<GuestMemory>* rx_ = nullptr;
    std::vector<uint8_t> pending_;  
};