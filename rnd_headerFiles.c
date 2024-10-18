
#define PRNG_FEEDBACK 0xe0000200

int RNG_calls(void);

int parity(unsigned int a0);

void set_PRNG_seed(unsigned int );

unsigned int PRNG_value(int);

unsigned int PRNG_value2(int, unsigned int *);
unsigned int PRNG_value2_32(unsigned int *);
