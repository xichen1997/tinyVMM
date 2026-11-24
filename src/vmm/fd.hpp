#pragma once
// RAII owner of a file descriptor: closes on destruction, move-only
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <string>
#include <utility>

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
