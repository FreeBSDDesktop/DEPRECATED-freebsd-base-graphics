
#include <sys/types.h>
#include <machine/cpufunc.h>

typedef uint64_t cycles_t;

static inline cycles_t get_cycles(void)
{
	return rdtsc();
}
