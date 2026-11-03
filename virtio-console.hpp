#include "virtio.hpp"
#include "mmiobus.hpp"
#include <cstring>
#include <iostream>

class VirtioConsole : public VirtioDevice {
private:
    uint32_t device_id_ = 3;
    uint64_t features_ = 0;
    int num_queues_ = 1;
    uint8_t name[32] = "virtio-console";
    uint8_t status = 0;
    uint8_t max_vq_pairs = 1;
    uint8_t num_vq_pairs = 1;
    uint8_t queue_size = 128;
public:
    uint32_t device_id() override { return device_id_; }
    uint64_t features() override { return features_; }
    int num_queues() override { return num_queues_; }
    void read_config(uint64_t offset, uint8_t* data, uint32_t len) override {
        if (offset == 0) {
            data[0] = 0;
            data[1] = 0;
            data[2] = 0;
            data[3] = 0;
        } else if (offset == 1) {
            data[0] = 0;
            data[1] = 0;
        } else if (offset == 2) {
            data[0] = 0;
            data[1] = 0;
        } else if (offset == 3) {
            data[0] = 0;
            data[1] = 0;
        } else if (offset == 4) {
            data[0] = 0;
            data[1] = 0;
    }
}
    void process_queue(int q) override {
        std::cout << "VirtioConsole: process_queue(" << q << ")" << std::endl;
    }
};
