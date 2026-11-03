#pragma once

#include <sys/mman.h>     // mmap / munmap / PROT_* / MAP_*
#include <cerrno>         // errno
#include <cstdint>
#include <cstring>        // memcpy
#include <span>
#include <stdexcept>      // out_of_range
#include <system_error>   // system_error
#include <utility>        // exchange


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