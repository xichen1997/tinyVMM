#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include <iostream>
#include <unistd.h>
#include <string>
#include <system_error>
#include <cerrno>
#include <memory>
#include <utility>
#include <vector>
#include <algorithm>
#include <cstring>
#include <map>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <fstream>
#include <csignal>
#include <mutex>
#include <thread>


#include "vmm/fd.hpp"
#include "vmm/gdt.hpp"

#include "vmm/guest_memory.hpp"

// bzimage parser and boot_params builder
#include "boot/bzimage.hpp"
#include "boot/boot_params.hpp"

// I/O bus
#include "bus/iobus.hpp"
#include "devices/serial8250.hpp"
#include "vmm/terminal.hpp"
#include "devices/i8042.hpp"

// MMIO bus
#include "bus/mmiobus.hpp"

// virtio, virtio already includes virtqueue.hpp
#include "virtio/virtio.hpp"
#include "virtio/virtio-console.hpp"
#include "virtio/virtio-blk.hpp"

static void on_sigint(int) {}   

struct Region {
    uint64_t start;
    uint64_t size;
    constexpr uint64_t end() const { return start + size; }
};


constexpr bool overlaps(Region a, Region b) {
    return a.start < b.end() && b.start < a.end();
}

namespace layout {
    constexpr uint64_t MEM_SIZE = 256ull << 20; // 256 MiB, the kernel init_size needs 56 MB

    constexpr Region GDT  {0x500, 0x100};
    constexpr Region PML4 {0x1000, 0x1000};
    constexpr Region PDPT {0x2000, 0x1000};
    constexpr Region PD   {0x3000, 0x1000};

    constexpr Region CODE {0x10000, 0x10000};
    constexpr Region BOOT_PARAMS {0x7000, 0x1000};  // boot_params is at 0x7000
    constexpr Region STACK {0x30000, 0x10000};
    constexpr Region CMDLINE {0x20000,  0x1000};  // cmdline is at 0x20000
    constexpr Region KERNEL {0x100000, 0x4000000};  // kernel is at 0x100000, 64 MB is enough for the test case

    constexpr Region ALL[] = {GDT, PML4, PDPT, PD, CODE, BOOT_PARAMS, STACK, CMDLINE, KERNEL};  // all regions  
}

constexpr bool no_overlaps() {
    for(size_t i = 0; i < std::size(layout::ALL); i++) {
        for(size_t j = i + 1; j < std::size(layout::ALL); j++) {
            if(overlaps(layout::ALL[i], layout::ALL[j])){
                return false;
            }
        }
    }
    return true;
}

constexpr bool all_in_memory() {
    for (Region r : layout::ALL)
        if (r.end() > layout::MEM_SIZE) return false;
    return true;
}


class KVM {
public:
    KVM() : fd_(::open("/dev/kvm", O_RDWR | O_CLOEXEC)) {
        if (fd_.get() < 0)
            throw std::system_error(errno, std::generic_category(), "open /dev/kvm");
        if (::ioctl(fd_.get(), KVM_GET_API_VERSION, 0) != 12)
            throw std::runtime_error("unexpected KVM API version");
    }

    int get() const { return fd_.get(); }

private:
    FD fd_;
};

class VM {
public:
    VM(const KVM& kvm, size_t mem_size, uint64_t guest_addr)
        : mem_(mem_size),
          fd_(::ioctl(kvm.get(), KVM_CREATE_VM, 0)) {
        if (fd_.get() < 0)
            throw std::system_error(errno, std::generic_category(), "KVM_CREATE_VM");

        kvm_userspace_memory_region region{
            .slot = 0,
            .guest_phys_addr = guest_addr,
            .memory_size = mem_size,
            .userspace_addr = reinterpret_cast<uint64_t>(mem_.host_addr()),
        };
        if (::ioctl(fd_.get(), KVM_SET_USER_MEMORY_REGION, &region) < 0)
            throw std::system_error(errno, std::generic_category(), "KVM_SET_USER_MEMORY_REGION");
    }

    int get() const { return fd_.get(); }
    GuestMemory& mem() { return mem_; }  // use to copy code to guest memory

private:
    GuestMemory mem_;
    FD fd_;
};

class VCPU{
public:
    VCPU(const KVM& kvm, const VM& vm):
    fd_(create(vm)),
    run_(::ioctl(kvm.get(), KVM_GET_VCPU_MMAP_SIZE, 0), PROT_READ | PROT_WRITE, MAP_SHARED, fd_.get()) {}
    ~VCPU() = default;
    int get() const { return fd_.get(); }
    int run() const {
        return ::ioctl(fd_.get(), KVM_RUN, 0);
    }
    kvm_run* state() const { return reinterpret_cast<kvm_run*>(run_.data()); }

private:
    static int create(const VM& vm){
        int fd = ::ioctl(vm.get(), KVM_CREATE_VCPU, 0);
        if(fd < 0){
            throw std::system_error(errno, std::generic_category(), "KVM_CREATE_VCPU");
        }
        return fd;
    }
    FD fd_;
    Mmap run_;
};

const char* exit_name(uint32_t reason) {
    switch (reason) {
    case KVM_EXIT_IO: return "KVM_EXIT_IO";
    case KVM_EXIT_HLT: return "KVM_EXIT_HLT";
    case KVM_EXIT_MMIO: return "KVM_EXIT_MMIO";
    case KVM_EXIT_INTR: return "KVM_EXIT_INTR";
    case KVM_EXIT_SHUTDOWN: return "KVM_EXIT_SHUTDOWN";
    case KVM_EXIT_FAIL_ENTRY: return "KVM_EXIT_FAIL_ENTRY";
    case KVM_EXIT_INTERNAL_ERROR: return "KVM_EXIT_INTERNAL_ERROR";
    default: return "KVM_EXIT_OTHER";
    }
}

int main(int argc, char** argv) {
    // static_assert to check the conditions
    static_assert(no_overlaps(), "guest memory regions overlap");
    static_assert(all_in_memory(), "region exceeds guest memory");
    static_assert(layout::PML4.start % 0x1000 == 0, "page tables must be 4 KB aligned");
    static_assert(layout::PDPT.start % 0x1000 == 0, "page tables must be 4 KB aligned");
    static_assert(layout::PD.start   % 0x1000 == 0, "page tables must be 4 KB aligned");


    bool trace_exits = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--trace-exits") == 0)
            trace_exits = true;
        else {
            std::fprintf(stderr, "usage: %s [--trace-exits]\n", argv[0]);
            return 2;
        }
    }

    KVM kvm;
    VM vm(kvm, layout::MEM_SIZE, 0);

    // Linux needs an interrupt controller and a timer; KVM emulates them in the kernel.
    // These must be created before the vCPU.
    if (ioctl(vm.get(), KVM_SET_TSS_ADDR, 0xfffbd000) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_SET_TSS_ADDR");
    if (ioctl(vm.get(), KVM_CREATE_IRQCHIP, 0) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_CREATE_IRQCHIP");
    // SPEAKER_DUMMY makes KVM also emulate port 0x61, which exposes PIT channel 2's output.
    // Without it, Linux spins forever reading 0x61 during TSC calibration.
    kvm_pit_config pit{};
    pit.flags = KVM_PIT_SPEAKER_DUMMY;
    if (ioctl(vm.get(), KVM_CREATE_PIT2, &pit) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_CREATE_PIT2");

    // Create the vCPU.
    VCPU vcpu(kvm, vm);

    // Set up the vCPU CPUID.
    // Tell the guest which CPU features exist; without this Linux thinks there is no 64-bit.
    //
    // In C++, sizeof(kvm_cpuid2) is 12 (flex array quirks) but the kernel/C ABI is 8
    // (nent + padding). Using the header macros then encodes the wrong ioctl size → EINVAL.
    // Use a fixed layout + ioctl numbers with an 8-byte type instead.
    struct Cpuid2 {
        uint32_t nent;
        uint32_t padding;
        kvm_cpuid_entry2 entries[256];
    } cpuid{};
    cpuid.nent = 256;
    constexpr auto GET_SUPPORTED_CPUID = _IOWR(KVMIO, 0x05, uint64_t);  // size 8
    constexpr auto SET_CPUID2          = _IOW(KVMIO, 0x90, uint64_t);   // size 8
    if (ioctl(kvm.get(), GET_SUPPORTED_CPUID, &cpuid) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_GET_SUPPORTED_CPUID");
    if (ioctl(vcpu.get(), SET_CPUID2, &cpuid) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_SET_CPUID2");

    // No BIOS here to set up "virtual wire" mode, so do it ourselves:
    // LINT0 receives PIC interrupts (ExtINT), LINT1 receives NMI.
    kvm_lapic_state lapic{};
    if (ioctl(vcpu.get(), KVM_GET_LAPIC, &lapic) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_GET_LAPIC");

    auto set_delivery_mode = [&](size_t reg, uint32_t mode) {
        uint32_t v;
        std::memcpy(&v, lapic.regs + reg, sizeof(v));
        v = (v & ~0x700u) | (mode << 8);              // bits 8-10: delivery mode
        std::memcpy(lapic.regs + reg, &v, sizeof(v));
    };
    set_delivery_mode(0x350, 7);   // LVT LINT0 -> ExtINT
    set_delivery_mode(0x360, 4);   // LVT LINT1 -> NMI

    if (ioctl(vcpu.get(), KVM_SET_LAPIC, &lapic) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_SET_LAPIC");


    GuestMemory& mem = vm.mem();

    // load kernel.bin
    BzImage img = load_bzimage("kernel.bin");
    std::string cmdline = "console=hvc0 reboot=k panic=1 pci=off rdinit=/init virtio_mmio.device=4K@0xd0000000:5 virtio_mmio.device=4K@0xd0001000:6";

    mem.write(layout::KERNEL.start, img.kernel.data(), img.kernel.size());
    mem.write(layout::CMDLINE.start, cmdline.c_str(), cmdline.size() + 1);   // with '\0'


    // load initramfs.cpio.gz
    std::ifstream f("initramfs.cpio.gz", std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot open initramfs.cpio.gz");
    std::vector<uint8_t> initrd{std::istreambuf_iterator<char>(f), {}};
    uint64_t initrd_addr = (layout::MEM_SIZE - initrd.size()) & ~0xfffull;   // top of memory, 4 KiB aligned
    mem.write(initrd_addr, initrd.data(), initrd.size());

    auto params = build_boot_params(img, layout::MEM_SIZE, cmdline, layout::CMDLINE.start,
                                    initrd_addr, initrd.size());


    mem.write(layout::BOOT_PARAMS.start, params.data(), params.size());

    SegmentDesc code_desc{.limit = 0xFFFFF, .type = 0xA, .s = 1, .present = 1, .l = 1, .g = 1};
    SegmentDesc data_desc{.limit = 0xFFFFF, .type = 0x2, .s = 1, .present = 1, .db = 1, .g = 1};

    mem.write<uint64_t>(layout::GDT.start + 0,  0);
    mem.write<uint64_t>(layout::GDT.start + 8,  0);
    mem.write<uint64_t>(layout::GDT.start + 16, encode(code_desc)); // selector 0x10
    mem.write<uint64_t>(layout::GDT.start + 24, encode(data_desc)); // selector 0x18

    kvm_sregs sregs{};
    if (ioctl(vcpu.get(), KVM_GET_SREGS, &sregs) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_GET_SREGS");

    mem.write<uint64_t>(layout::PML4.start, layout::PDPT.start | 0x3);
    mem.write<uint64_t>(layout::PDPT.start, layout::PD.start | 0x3);
    for (uint64_t i = 0; i < 512; i++)
    mem.write<uint64_t>(layout::PD.start + i * 8, (i << 21) | 0x83);

    sregs.cr3  = layout::PML4.start;
    sregs.cr4  = 1 << 5;                  // PAE (page address extension), which is required for long mode.
    // Long mode is enabled by setting the Long Mode Enable (LME) and Long Mode Active (LMA) bits in the EFER register.
    sregs.efer = (1 << 8) | (1 << 10);    // LME | LMA (Long Mode Enable | Long Mode Active)
    sregs.cr0  = (1u << 31) | 1;          // PG | PE (Paging Enable | Protection Enable)
    sregs.gdt.base  = layout::GDT.start;
    sregs.gdt.limit = 4 * 8 - 1;
    sregs.cs = to_kvm_segment(code_desc, 0x10);   
    sregs.ds = sregs.es = sregs.ss = to_kvm_segment(data_desc, 0x18);


    if (ioctl(vcpu.get(), KVM_SET_SREGS, &sregs) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_SET_SREGS");

    kvm_regs regs{};
    regs.rip = layout::KERNEL.start + 0x200; // virtual address of kernel entry point, 64 bit mode requires 0x200 offset
    regs.rsi = layout::BOOT_PARAMS.start;      // boot protocol: rsi = boot_params
    regs.rsp = layout::STACK.end(); // virtual address of stack


    if (ioctl(vcpu.get(), KVM_SET_REGS, &regs) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_SET_REGS");

    kvm_run* state = vcpu.state();
    std::map<uint32_t, uint64_t> exits;
    auto dump_exits = [&] {
        if (!trace_exits)
            return;
        std::fflush(stdout);
        std::fprintf(stderr, "exits:\n");
        for (const auto& [reason, count] : exits)
            std::fprintf(stderr, "  %s (%u): %llu\n", exit_name(reason), reason,
                         static_cast<unsigned long long>(count));
    };

    // COM1 serial port, all port accesses go through IoBus
    Serial8250 serial;
    IoBus io;

    // This is still needed for seeing the booting logs before virtio console is ready.
    io.add(0x3f8, 8, &serial);

    I8042 kbd;
    io.add(0x60, 5, &kbd);   // 0x60 ~ 0x64; 0x61 is handled by KVM in the kernel, so it won't reach here

    // MMIO bus for the device that is not mapped to the guest memory
    MmioBus mmio;

    // input thread and vCPU loop both access the serial port, use a lock to chain them
    std::mutex dev_lock;
    bool irq4 = false;
    auto update_irq = [&] {                   // must hold dev_lock when calling
        bool level = serial.irq_pending();
        if (level == irq4)
            return;
        kvm_irq_level line{};
        line.irq = 4;                         // COM1 uses IRQ 4
        line.level = level;
        if (ioctl(vm.get(), KVM_IRQ_LINE, &line) < 0)
            throw std::system_error(errno, std::generic_category(), "KVM_IRQ_LINE");
        irq4 = level;
    };

    bool irq5 = false;
    auto raise_irq5 = [&](bool level) {        // must hold dev_lock when calling
        if (level == irq5)
            return;
        kvm_irq_level line{};
        line.irq = 5;                          // virtio console uses IRQ 5
        line.level = level;
        if (ioctl(vm.get(), KVM_IRQ_LINE, &line) < 0)
            throw std::system_error(errno, std::generic_category(), "KVM_IRQ_LINE");
        irq5 = level;
    };

    bool irq6 = false;
    auto raise_irq6 = [&](bool level) {        // must hold dev_lock when calling
        if (level == irq6) return;
        kvm_irq_level line{};
        line.irq = 6;                           // virtio-blk uses IRQ 6
        line.level = level;
        if (ioctl(vm.get(), KVM_IRQ_LINE, &line) < 0)
            throw std::system_error(errno, std::generic_category(), "KVM_IRQ_LINE");
        irq6 = level;
    };

    VirtioConsole console;
    VirtioBlk blk(FD::open_file("disk.img", O_RDWR));
    VirtioMmioTransport transport_console(console, raise_irq5, mem);
    VirtioMmioTransport transport_blk(blk, raise_irq6, mem);
    mmio.add(0xd0000000, 0x1000, &transport_console);
    mmio.add(0xd0001000, 0x1000, &transport_blk);

    // set stdin to raw mode, each key is directly passed to the guest. press Ctrl-A then x to exit.
    RawTerminal term;
    std::thread([&] {
        bool ctrl_a = false;
        uint8_t c;
        while (::read(STDIN_FILENO, &c, 1) == 1) {
            if (ctrl_a && c == 'x') {
                term.restore();
                std::_Exit(0);
            }
            ctrl_a = (c == 0x01);
            std::lock_guard<std::mutex> g(dev_lock);
            try {
                if (console.push_input(&c, 1))
                    transport_console.signal_used();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[virtio] rx: %s\n", e.what());
                transport_console.needs_reset();
            }
        }
    }).detach();

    // signal handler8
    struct sigaction sa{};
    sa.sa_handler = on_sigint;   // without SA_RESTART, KVM_RUN will return EINTR
    sigaction(SIGINT, &sa, nullptr);
    
    while (true) {
        if (vcpu.run() < 0) {
            if (errno != EINTR)
                throw std::system_error(errno, std::generic_category(), "KVM_RUN");
            kvm_regs r{};
            ioctl(vcpu.get(), KVM_GET_REGS, &r);
            std::fprintf(stderr, "\n[Ctrl-C] rip=%016llx rsp=%016llx\n", r.rip, r.rsp);
            dump_exits();
            continue;              
        }

        uint32_t reason = state->exit_reason;
        if (trace_exits)
            ++exits[reason];
        switch (reason) {
        case KVM_EXIT_IO: {
            std::lock_guard<std::mutex> g(dev_lock);
            io.handle(state);
            update_irq();
            if (kbd.reset_requested()) {
                std::fprintf(stderr, "\n[vmm] guest requested reset, exiting\n");
                dump_exits();
                return 0;
            }
            break;
        }
        case KVM_EXIT_MMIO: {
            std::lock_guard<std::mutex> g(dev_lock);
            mmio.handle(state);
            break;
        }
        case KVM_EXIT_HLT:
            std::puts("KVM_EXIT_HLT");
            dump_exits();
            return 0;
        case KVM_EXIT_SHUTDOWN:
            std::fprintf(stderr, "\n[vmm] guest shutdown (triple fault), exiting\n");
            dump_exits();
            return 0;
        default:
            std::printf("unhandled exit %d\n", reason);
            dump_exits();
            return 1;
        }
    }
    return 0;
}