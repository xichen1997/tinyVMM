static void outb(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void putchar(char c) { outb(0x3f8, c); }

static void puts(const char* s) {
    while (*s) putchar(*s++);
}

static void hc_exit(unsigned char code) {
    outb(0xf4, code);
    for (;;) __asm__ volatile("hlt"); 
}

// entry point

void _start(void) {
    puts("Hello from C!\n");
    // exit with code 0
    hc_exit(0);
}