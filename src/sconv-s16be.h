#ifndef foosconv_s16befoo
#define foosconv_s16befoo

void pa_sconv_s16be_to_float32(unsigned n, const void *a, unsigned an, float *b);
void pa_sconv_s16be_from_float32(unsigned n, const float *a, void *b, unsigned bn);

#endif
