#pragma once

#include <cstdint>
#include "iobus.hpp"

// minimal PS/2 keyboard controller (i8012) for one thing: guest uses it to request a reset.
// registered at 0x60: offset 0 = 0x60 (data), offset 4 = 0x64 (status/command).
// 0x61 is the PIT speaker port, which is handled by KVM in the kernel, so it won't reach here.

class I8042 : public IoDevice {
public:
    bool reset_requested() const { return reset_; }

    uint8_t read(uint16_t) override {
        return 0;   // status register 0: input and output buffers are empty, guest doesn't need to wait
    }

    void write(uint16_t offset, uint8_t v) override {
        if (offset == 4 && v == 0xFE)   // command 0xFE: pull the reset line of the CPU
            reset_ = true;
    }

private:
    bool reset_ = false;
};