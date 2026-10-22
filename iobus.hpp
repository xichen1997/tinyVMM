#pragma once

#include <linux/kvm.h>
#include <cstdint>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <vector>

// A device that occupies several I/O ports. offset = port - device's starting port.
struct IoDevice {
    virtual uint8_t read(uint16_t offset) = 0;
    virtual void write(uint16_t offset, uint8_t value) = 0;
    virtual ~IoDevice() = default;
};

// port → device's map
class IoBus {
public:
    void add(uint16_t base, uint16_t len, IoDevice* dev) {
        for (const Entry& e : entries_)
            if (base < e.base + e.len && e.base < base + len)
                throw std::runtime_error("IoBus: port ranges overlap");
        entries_.push_back({base, len, dev});
    }

    // Handle one KVM_EXIT_IO
    void handle(kvm_run* run) {
        uint8_t* data = reinterpret_cast<uint8_t*>(run) + run->io.data_offset;
        bool out = run->io.direction == KVM_EXIT_IO_OUT;

        // when count > 1 for rep ins/outs; when size = 2 for inw/outw, process adjacent ports byte by byte
        for (uint32_t i = 0; i < run->io.count; i++) {
            for (uint8_t b = 0; b < run->io.size; b++) {
                uint16_t port = run->io.port + b;
                uint8_t& byte = data[i * run->io.size + b];
                if (out)
                    write(port, byte);
                else
                    byte = read(port);
            }
        }
    }

private:
    struct Entry {
        uint16_t base;
        uint16_t len;
        IoDevice* dev;
    };

    uint8_t read(uint16_t port) {
        for (const Entry& e : entries_)
            if (port >= e.base && port < e.base + e.len)
                return e.dev->read(port - e.base);
        log_unknown("read", port);
        return 0xFF;   // no device on the port: no one is driving it, read is all 1
    }

    void write(uint16_t port, uint8_t value) {
        for (const Entry& e : entries_)
            if (port >= e.base && port < e.base + e.len)
                return e.dev->write(port - e.base, value);
        log_unknown("write", port);   // write is discarded
    }

    // each port is logged only once, otherwise the kernel will spam the log when it repeatedly probes
    void log_unknown(const char* what, uint16_t port) {
        if (logged_.insert(port).second)
            std::fprintf(stderr, "[io] unhandled %s port %#x\n", what, port);
    }

    std::vector<Entry> entries_;
    std::set<uint16_t> logged_;
};