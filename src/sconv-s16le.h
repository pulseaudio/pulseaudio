#ifndef foosconv_s16lefoo
#define foosconv_s16lefoo

void pa_sconv_s16le_to_float32(unsigned n, const void *a, unsigned an, float *b);
void pa_sconv_s16le_from_float32(unsigned n, const float *a, void *b, unsigned bn);

#endif
