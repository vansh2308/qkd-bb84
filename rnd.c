


#include "rnd.h"

int __RNG_calls = 0; /* for test purposes */


/* takes less than 17 nsec on my laptop */
int parity(unsigned int a) {
    int b;
    int c,d0;
    asm  ("movl $0,%2\n"
	  "\tmovl %3,%0\n"
	  "\tmovl %0,%1\n"
	  "\tshrl $16,%1\n"
	  "\txorl %0,%1\n"
	  "\tmovl %1,%0\n"
	  "\tshrl $8,%1\n"
	  "\txorl %1,%0\n"
	  "\tjpe 1f\n"
	  "\tmovl $1,%2\n"
	  "1:"
	  : "=&a" (d0), "=&D" (c), "=&c" (b)
	  : "d" (a)
	);
    return b;
} 

/* this is an implementation of an m-sequence */

/* PSRNG fuction which sets a seed */
unsigned int __PRNG_state;
void set_PRNG_seed(unsigned int seed) {
    __PRNG_state=seed;
}
/* get k bits from PSRNG */
unsigned int PRNG_value(int k) {
    int k0;
    int b;
    for (k0=k;k0;k0--) {
	b=parity(__PRNG_state & PRNG_FEEDBACK);
	__PRNG_state <<= 1; __PRNG_state += b;
    }
    return ((1<<k)-1) & __PRNG_state;
}

/* version which iterates the PRNG from a given state location */
unsigned int PRNG_value2(int k, unsigned int *state) {
    int k0;
    int b;
    for (k0=k;k0;k0--) {
	b=parity( *state & PRNG_FEEDBACK);
	*state <<= 1; *state += b;
    }
    __RNG_calls++;
    return ((1<<k)-1) & *state;
}
unsigned int PRNG_value2_32(unsigned int *state) {
    int k0;
    int b;
    for (k0=32;k0;k0--) {
	b=parity( *state & PRNG_FEEDBACK);
	*state <<= 1; *state += b;
    }
    __RNG_calls++;
    return *state;
}


int RNG_calls(void) {return __RNG_calls;};
