// itoa and utoa come from newlib on the device; the host C library has neither.
#include "stdlib_noniso.h"
char* itoa(int val, char* s, int radix) { return ltoa(val, s, radix); }
char* utoa(unsigned int val, char* s, int radix) { return ultoa(val, s, radix); }
