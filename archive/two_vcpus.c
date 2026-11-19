#include <err.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* One VM, two vCPUs.  Each vCPU is a host thread blocked in KVM_RUN.
 * The guest snippets only OUT one character; a guest OS would schedule
 * its own threads on these same vCPUs without returning from KVM_RUN.
 */

struct vcpu {
    int fd;
    struct kvm_run *run;
    char ch;
};

static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;

static void setup_vcpu(int kvm, int vmfd, struct vcpu *vcpu, int id, uint64_t rip)
{
    int ret;
    struct kvm_sregs sregs;
    struct kvm_regs regs;

    vcpu->fd = ioctl(vmfd, KVM_CREATE_VCPU, (unsigned long)id);
    if (vcpu->fd == -1)
        err(1, "KVM_CREATE_VCPU %d", id);

    ret = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, NULL);
    if (ret == -1)
        err(1, "KVM_GET_VCPU_MMAP_SIZE");
    vcpu->run = mmap(NULL, ret, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu->fd, 0);
    if (vcpu->run == MAP_FAILED)
        err(1, "mmap vcpu %d", id);

    if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) == -1)
        err(1, "KVM_GET_SREGS");
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) == -1)
        err(1, "KVM_SET_SREGS");

    memset(&regs, 0, sizeof(regs));
    regs.rip = rip;
    regs.rflags = 0x2;
    if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) == -1)
        err(1, "KVM_SET_REGS");
}

static void *vcpu_thread(void *arg)
{
    struct vcpu *vcpu = arg;

    for (;;) {
        if (ioctl(vcpu->fd, KVM_RUN, NULL) == -1)
            err(1, "KVM_RUN");

        switch (vcpu->run->exit_reason) {
        case KVM_EXIT_HLT:
            return NULL;
        case KVM_EXIT_IO:
            if (vcpu->run->io.direction == KVM_EXIT_IO_OUT &&
                vcpu->run->io.port == 0x3f8) {
                char *data = (char *)vcpu->run + vcpu->run->io.data_offset;
                pthread_mutex_lock(&io_lock);
                putchar(*data);
                fflush(stdout);
                pthread_mutex_unlock(&io_lock);
            } else {
                errx(1, "unhandled KVM_EXIT_IO");
            }
            break;
        default:
            errx(1, "exit_reason = 0x%x", vcpu->run->exit_reason);
        }
    }
}

int main(void)
{
    int kvm, vmfd, ret;
    uint8_t *mem;
    struct vcpu vcpus[2];
    pthread_t threads[2];

    /* mov $0x3f8, %dx; mov $'A', %al; out; mov $'\n', %al; out; hlt */
    const uint8_t code_a[] = {
        0xba, 0xf8, 0x03, 0xb0, 'A', 0xee, 0xb0, '\n', 0xee, 0xf4,
    };
    const uint8_t code_b[] = {
        0xba, 0xf8, 0x03, 0xb0, 'B', 0xee, 0xb0, '\n', 0xee, 0xf4,
    };

    kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm == -1)
        err(1, "/dev/kvm");

    vmfd = ioctl(kvm, KVM_CREATE_VM, (unsigned long)0);
    if (vmfd == -1)
        err(1, "KVM_CREATE_VM");

    mem = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        err(1, "mmap guest memory");
    memcpy(mem, code_a, sizeof(code_a));
    memcpy(mem + 0x1000, code_b, sizeof(code_b));

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0x1000,
        .memory_size = 0x2000,
        .userspace_addr = (uint64_t)mem,
    };
    ret = ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);
    if (ret == -1)
        err(1, "KVM_SET_USER_MEMORY_REGION");

    vcpus[0].ch = 'A';
    vcpus[1].ch = 'B';
    setup_vcpu(kvm, vmfd, &vcpus[0], 0, 0x1000);
    setup_vcpu(kvm, vmfd, &vcpus[1], 1, 0x2000);

    /* Two host threads, one VM.  Guest threads would be scheduled by the
     * guest kernel onto these vCPUs and would not show up here. */
    if (pthread_create(&threads[0], NULL, vcpu_thread, &vcpus[0]) != 0)
        err(1, "pthread_create");
    if (pthread_create(&threads[1], NULL, vcpu_thread, &vcpus[1]) != 0)
        err(1, "pthread_create");

    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    return 0;
}
