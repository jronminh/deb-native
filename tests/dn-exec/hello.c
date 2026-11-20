/* A freestanding static hello for the dn-exec POC. */
static long sc3(long n, long a, long b, long c) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile(
        "mov x0, #1\n"
        "adr x1, msg\n"
        "mov x2, #6\n"
        "mov x8, #64\n"
        "svc 0\n"
        "mov x0, #0\n"
        "mov x8, #93\n"
        "svc 0\n"
        "msg: .ascii \"hello\\n\"\n");
}
