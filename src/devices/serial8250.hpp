#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>

#include "bus/iobus.hpp"

// 16550A compatible serial port.
// register offset (relative to 0x3f8):
//   0 RBR/THR (when DLAB=1: DLL)   1 IER (when DLM=1: DLM)
//   2 IIR / FCR   3 LCR   4 MCR   5 LSR   6 MSR   7 SCR
//
// this class does not touch KVM: VMM calls irq_pending() before each access, and sets the result to IRQ 4.
class Serial8250 : public IoDevice {
public:
    explicit Serial8250(std::function<void(uint8_t)> out = [](uint8_t c) {
        std::fputc(c, stdout);
        std::fflush(stdout);
    })
        : out_(std::move(out)) {}

    // VMM calls push_input() when it receives keyboard input
    void push_input(uint8_t c) { rx_.push_back(c); }

    // whether to raise the interrupt line. OUT2 bit of MCR is the total switch for interrupt output on the PC.
    bool irq_pending() const { return (mcr_ & 0x08) && interrupt_id() != 0x01; }

    uint8_t read(uint16_t offset) override {
        switch (offset) {
        case 0:
            if (dlab())
                return dll_;
            if (rx_.empty())
                return 0;
            {
                uint8_t c = rx_.front();   // RBR: take one received byte
                rx_.pop_front();
                return c;
            }
        case 1: return dlab() ? dlm_ : ier_;
        case 2: {
            uint8_t id = interrupt_id();
            if (id == 0x02)
                thre_pending_ = false;    // read "transmit empty" interrupt, consider it handled
            return id | ((fcr_ & 0x01) ? 0xC0 : 0);   // if FIFO is enabled, report 16550A
        }
        case 3: return lcr_;
        case 4: return mcr_;
        case 5: return 0x60 | (rx_.empty() ? 0 : 0x01);   // THRE | TEMT, add DR when there is data
        case 6: return msr();
        case 7: return scr_;
        }
        return 0xFF;
    }

    void write(uint16_t offset, uint8_t v) override {
        switch (offset) {
        case 0:
            if (dlab()) {
                dll_ = v;
            } else {
                if (!(mcr_ & 0x10))       // in loopback mode, do not send out
                    out_(v);
                thre_pending_ = true;     // character is sent immediately, transmitter buffer is empty
            }
            break;
        case 1:
            if (dlab()) {
                dlm_ = v;
            } else {
                ier_ = v & 0x0F;
                if (ier_ & 0x02)          // when "transmit empty" interrupt is enabled, buffer is already empty, trigger immediately
                    thre_pending_ = true;
            }
            break;
        case 2: fcr_ = v; break;
        case 3: lcr_ = v; break;
        case 4: mcr_ = v & 0x1F; break;
        case 7: scr_ = v; break;
        // 5 (LSR)、6 (MSR) are read only, writes are ignored
        }
    }

private:
    bool dlab() const { return lcr_ & 0x80; }

    // IIR lowest 4 bits: current highest priority interrupt. 0x04: received data, 0x02: transmit empty, 0x01: no interrupt
    uint8_t interrupt_id() const {
        if ((ier_ & 0x01) && !rx_.empty())
            return 0x04;
        if ((ier_ & 0x02) && thre_pending_)
            return 0x02;
        return 0x01;
    }

    // in loopback mode, MSR highest 4 bits reflect MCR lowest 4 bits, Linux checks this when probing the serial port
    uint8_t msr() const {
        if (mcr_ & 0x10)
            return ((mcr_ & 0x01) << 5) |   // DTR  -> DSR
                   ((mcr_ & 0x02) << 3) |   // RTS  -> CTS
                   ((mcr_ & 0x04) << 4) |   // OUT1 -> RI
                   ((mcr_ & 0x08) << 4);    // OUT2 -> DCD
        return 0xB0;                         // DCD | DSR | CTS：对端在线
    }

    std::function<void(uint8_t)> out_;
    std::deque<uint8_t> rx_;                 // 收到、还没被 guest 读走的字节
    bool thre_pending_ = false;
    uint8_t ier_ = 0, fcr_ = 0, lcr_ = 0, mcr_ = 0, scr_ = 0;
    uint8_t dll_ = 0x0C, dlm_ = 0;           // 默认 9600 波特
};