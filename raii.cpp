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
        : mem_(mem_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS),
            fd_(::ioctl(kvm.get(), KVM_CREATE_VM, 0)) {
        if (fd_.get() < 0)
            throw std::system_error(errno, std::generic_category(), "KVM_CREATE_VM");

        kvm_userspace_memory_region region{
            .slot = 0,
            .guest_phys_addr = guest_addr,
            .memory_size = mem_size,
            .userspace_addr = reinterpret_cast<uint64_t>(mem_.data()),
        };
        if (::ioctl(fd_.get(), KVM_SET_USER_MEMORY_REGION, &region) < 0)
            throw std::system_error(errno, std::generic_category(), "KVM_SET_USER_MEMORY_REGION");
    }

    int get() const { return fd_.get(); }
    void* mem() const { return mem_.data(); }   // use to copy code to guest memory

private:
    Mmap mem_;  
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
    VM vm(kvm, 0x1000, 0x1000);
    VCPU vcpu(kvm, vm);

    // Guest is in real mode. Bytes below are 16-bit code, not assembly text.
    // Guest physical 0x1000 is mem()[0]. String sits at guest 0x1020.
    auto* mem = reinterpret_cast<uint8_t*>(vm.mem());
    std::strcpy(reinterpret_cast<char*>(mem + 0x20), "Hello, World!");

    const uint8_t code[] = {
        0xba, 0xf8, 0x04,       // mov $0x3f8, %dx
        0xbe, 0x20, 0x10,       // mov $0x1020, %si
        0xb9, 0x0d, 0x00,       // mov $13, %cx
        0xac,                   // lodsb
        0xee,                   // out %al, (%dx)
        0xe2, 0xfc,             // loop lodsb
        0xb0, '\n',             // mov $'\n', %al
        0xee,                   // out %al, (%dx)
        0xf4,                   // hlt
    };
    std::memcpy(mem, code, sizeof(code));

    kvm_sregs sregs{};
    if (ioctl(vcpu.get(), KVM_GET_SREGS, &sregs) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_GET_SREGS");
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    sregs.ds.base = 0;
    sregs.ds.selector = 0;
    if (ioctl(vcpu.get(), KVM_SET_SREGS, &sregs) < 0)
        throw std::system_error(errno, std::generic_category(), "KVM_SET_SREGS");

    kvm_regs regs{};
    regs.rip = 0x1000;
    regs.rflags = 0x2;
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
        vcpu.run();
        uint32_t reason = state->exit_reason;
        if (trace_exits)
            ++exits[reason];
        switch (reason) {
        case KVM_EXIT_IO:
            if (state->io.direction == KVM_EXIT_IO_OUT && state->io.port == 0x4f8)
                putchar(*((char*)state + state->io.data_offset));
            break;
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