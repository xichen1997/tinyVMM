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
#include <cstdint>
#include <cstring>
#include <map>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <fstream>
#include "gdt.hpp"

struct Region {
    uint64_t start;
    uint64_t size;
    constexpr uint64_t end() const { return start + size; }
};


constexpr bool overlaps(Region a, Region b) {
    return a.start < b.end() && b.start < a.end();
}

namespace layout {
    constexpr uint64_t MEM_SIZE = 0x200000;

    constexpr Region GDT  {0x500, 0x100};
    constexpr Region PML4 {0x1000, 0x1000};
    constexpr Region PDPT {0x2000, 0x1000};
    constexpr Region PD   {0x3000, 0x1000};

    constexpr Region CODE {0x10000, 0x10000};
    constexpr Region STACK {0x30000, 0x10000};
    constexpr Region ALL[] = {GDT, PML4, PDPT, PD, CODE, STACK};
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


class FD {
    private:
    int fd;
    public:
    // constructor
    explicit FD(int fd) noexcept : fd(fd) {}

    // open file
    static FD open_file(const char* path, int flags) {
        int fd = ::open(path, flags | O_CLOEXEC);
        if (fd == -1) {
            throw std::runtime_error("Failed to open file: " + std::string(path));
        }
        return FD{fd};
    }

    // delete copy constructor and assignment operator
    FD(const FD&) = delete;
    FD& operator=(const FD&) = delete;

    // move constructor and assignment operator
    FD(FD&& o) noexcept : fd(std::exchange(o.fd, -1)) {}
    FD& operator=(FD&& o) noexcept {
        if (this != &o) {
            if (fd >= 0) ::close(fd);
            fd = std::exchange(o.fd, -1);
        }
        return *this;
    }
    // get fd
    int get() const { return fd; }
    
    // destructor
    ~FD() { 
        if (fd >= 0) ::close(fd); 
    }
};

class Mmap {
public:
    // fd pass -1 for anonymous mapping (e.g. guest memory)
    Mmap(size_t size, int prot, int flags, int fd = -1) : size_(size) {
        addr_ = ::mmap(nullptr, size, prot, flags, fd, 0);
        if (addr_ == MAP_FAILED) {   // return MAP_FAILED on failure, not nullptr
            throw std::system_error(errno, std::generic_category(), "mmap");
        }
    }
    
    ~Mmap() {
        if (addr_) ::munmap(addr_, size_);
    }
    
    // cannot copy: otherwise will munmap twice
    Mmap(const Mmap&) = delete;
    Mmap& operator=(const Mmap&) = delete;
    
    // can move: ownership is transferred to the new object, the old object is empty
    Mmap(Mmap&& o) noexcept
        : addr_(std::exchange(o.addr_, nullptr)), size_(std::exchange(o.size_, 0)) {}
    
    // get pointer to the mapped memory
    void* data() const { return addr_; }
    // get size of the mapped memory
    size_t size() const { return size_; }
    
private:
    void* addr_ = nullptr;
    size_t size_ = 0;
};



class GuestMemory {
    public:
    GuestMemory(uint64_t size) : mem_(size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE) {}

    std::span<uint8_t> span(uint64_t gpa, uint64_t len) const {
        if (gpa > size() || len > size() - gpa)
            throw std::out_of_range("guest memory access out of range");
        return {static_cast<uint8_t*>(mem_.data()) + gpa, len};
    }
    
    // write two versions
    void write(uint64_t gpa, const void* src, uint64_t len) {
        std::memcpy(span(gpa, len).data(), src, len);
    }
    template <typename T>
    void write(uint64_t gpa, const T& v) {
        write(gpa, &v, sizeof(T));   
    }

    // read two versions
    void read(uint64_t gpa, void* dst, uint64_t len) const {
        std::memcpy(dst, span(gpa, len).data(), len);
    }
    template <typename T>
    T read(uint64_t gpa) const {
        T v;
        read(gpa, &v, sizeof(T));   
        return v;
    }

    void* host_addr() const { return mem_.data(); }
    uint64_t size() const { return mem_.size(); }

private:
    Mmap mem_;
};



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
    VCPU vcpu(kvm, vm);

    // Guest is in real mode. Code and string both live in CODE segment.
    // lodsb reads ds.base + si = CODE.start + 0x20.

    GuestMemory& mem = vm.mem();


    // load guest code 
    std::ifstream f("guest.bin", std::ios::binary);
    if (!f) {
        throw std::runtime_error("Failed to open guest.bin");
    }
    std::vector<char> bin{std::istreambuf_iterator<char>(f), {}};
    mem.write(layout::CODE.start, bin.data(), bin.size());
    f.close();

    SegmentDesc code_desc{.limit = 0xFFFFF, .type = 0xA, .s = 1, .present = 1, .l = 1, .g = 1};
    SegmentDesc data_desc{.limit = 0xFFFFF, .type = 0x2, .s = 1, .present = 1, .db = 1, .g = 1};

    mem.write<uint64_t>(layout::GDT.start + 0,  0);              
    mem.write<uint64_t>(layout::GDT.start + 8,  encode(code_desc));   
    mem.write<uint64_t>(layout::GDT.start + 16, encode(data_desc));   

    kvm_sregs sregs{};
    if (ioctl(vcpu.get(), KVM_GET_SREGS, &sregs) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_GET_SREGS");

    mem.write<uint64_t>(layout::PML4.start, layout::PDPT.start | 0x3);
    mem.write<uint64_t>(layout::PDPT.start, layout::PD.start | 0x3);
    for (uint64_t i = 0; i < 512; i++)
    mem.write<uint64_t>(layout::PD.start + i * 8, (i << 21) | 0x83);
    // mem.write<uint64_t>(layout::PD.start, 0x83);

    sregs.cr3  = layout::PML4.start;
    sregs.cr4  = 1 << 5;                  // PAE (page address extension), which is required for long mode.
    // Long mode is enabled by setting the Long Mode Enable (LME) and Long Mode Active (LMA) bits in the EFER register.
    sregs.efer = (1 << 8) | (1 << 10);    // LME | LMA (Long Mode Enable | Long Mode Active)
    sregs.cr0  = (1u << 31) | 1;          // PG | PE (Page Global Enable | Protection Enable)
    sregs.gdt.base  = layout::GDT.start;
    sregs.gdt.limit = 3 * 8 - 1;
    sregs.cs = to_kvm_segment(code_desc, 0x08);   
    sregs.ds = sregs.es = sregs.ss = to_kvm_segment(data_desc, 0x10);


    if (ioctl(vcpu.get(), KVM_SET_SREGS, &sregs) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_SET_SREGS");

    kvm_regs regs{};
    regs.rip = layout::CODE.start; // virtual address of code
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
    while (true) {
        if (vcpu.run() < 0) 
            throw std::system_error(errno, std::generic_category(), "KVM_RUN");
        uint32_t reason = state->exit_reason;
        if (trace_exits)
            ++exits[reason];
        switch (reason) {
        case KVM_EXIT_IO: {
            uint8_t* data = (uint8_t*)state + state->io.data_offset;
            if (state->io.direction == KVM_EXIT_IO_OUT && state->io.port == 0x3f8) {
                putchar(*data);
            } else if (state->io.direction == KVM_EXIT_IO_OUT && state->io.port == 0xf4) {
                dump_exits();
                return *data;
            }
            break;
        }
        case KVM_EXIT_HLT:
            std::puts("KVM_EXIT_HLT");
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