#include <cpuid.h>
#include <cstdio>
#include <cstring>

int main() {
    unsigned a, b, c, d;

    __get_cpuid(0, &a, &b, &c, &d);
    char vendor[13];
    std::memcpy(vendor, &b, 4);
    std::memcpy(vendor + 4, &d, 4);
    std::memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';
    std::printf("vendor: %s\n", vendor);

    __get_cpuid(1, &a, &b, &c, &d);
    std::printf("VMX: %u, in hypervisor: %u\n", (c >> 5) & 1, (c >> 31) & 1);

    __get_cpuid(0x80000001, &a, &b, &c, &d);
    std::printf("long mode: %u\n", (d >> 29) & 1);
}