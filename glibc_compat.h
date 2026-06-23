// GLIBC compatibility shim: provide __isoc23_* symbols that delegate to
// the original GLIBC functions. This allows the binary to run on systems
// with GLIBC < 2.38 even when OpenSSL static library references __isoc23_*.
#if defined(__linux__) && defined(__GLIBC__)

#include <stdlib.h>
#include <stdarg.h>

// OpenSSL 3.0.15 static library may reference __isoc23_strtol when compiled
// on GLIBC 2.38+ with _GNU_SOURCE defined. Provide an implementation that
// simply delegates to the standard strtol.
long __isoc23_strtol(const char* __restrict nptr, char** __restrict endptr, int base) {
    return strtol(nptr, endptr, base);
}

long long __isoc23_strtoll(const char* __restrict nptr, char** __restrict endptr, int base) {
    return strtoll(nptr, endptr, base);
}

unsigned long __isoc23_strtoul(const char* __restrict nptr, char** __restrict endptr, int base) {
    return strtoul(nptr, endptr, base);
}

unsigned long long __isoc23_strtoull(const char* __restrict nptr, char** __restrict endptr, int base) {
    return strtoull(nptr, endptr, base);
}

int __isoc23_sscanf(const char* __restrict s, const char* __restrict format, ...) {
    int result;
    va_list args;
    va_start(args, format);
    result = vsscanf(s, format, args);
    va_end(args);
    return result;
}

#endif
