#ifndef foosconvhfoo
#define foosconvhfoo

#include "sample.h"

typedef void (*convert_to_float32_func_t)(unsigned n, const void *a, unsigned an, float *b);
typedef void (*convert_from_float32_func_t)(unsigned n, const float *a, void *b, unsigned bn);

convert_to_float32_func_t get_convert_to_float32_function(enum pa_sample_format f);
convert_from_float32_func_t get_convert_from_float32_function(enum pa_sample_format f);



#endif
