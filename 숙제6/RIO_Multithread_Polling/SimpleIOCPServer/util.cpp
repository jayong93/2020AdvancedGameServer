#include <cstdlib>
#include "util.h"

float rand_float(float min, float max)
{
	return ((float)rand() / (float)RAND_MAX) * (max - min) + min;
}
