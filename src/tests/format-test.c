/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>

#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulse/format.h>
#include <pulse/xmalloc.h>

#define INIT(f) f = pa_format_info_new()
#define DEINIT(f) pa_format_info_free(f);
#define REINIT(f) { DEINIT(f); INIT(f); }

int main(int argc, char *argv[]) {
    pa_format_info *f1 = NULL, *f2 = NULL;
    int rates1[] = { 32000, 44100, 48000 }, i, temp_int1 = -1, temp_int2 = -1, *temp_int_array;
    const char *strings[] = { "thing1", "thing2", "thing3" };
    char *temp_str, **temp_str_array;

    /* 1. Simple fixed format int check */
    INIT(f1); INIT(f2);
    f1->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_int(f1, PA_PROP_FORMAT_RATE, 32000);
    f2->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_int(f2, PA_PROP_FORMAT_RATE, 44100);
    pa_assert(!pa_format_info_is_compatible(f1, f2));

    /* 2. Check int array membership - positive */
    REINIT(f1); REINIT(f2);
    f1->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_int_array(f1, PA_PROP_FORMAT_RATE, rates1, PA_ELEMENTSOF(rates1));
    f2->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_int(f2, PA_PROP_FORMAT_RATE, 44100);
    pa_assert(pa_format_info_is_compatible(f1, f2));
    pa_assert(pa_format_info_is_compatible(f2, f1));

    /* 3. Check int array membership - negative */
    REINIT(f2);
    f2->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_int(f2, PA_PROP_FORMAT_RATE, 96000);
    pa_assert(!pa_format_info_is_compatible(f1, f2));
    pa_assert(!pa_format_info_is_compatible(f2, f1));

    /* 4. Check int range - positive */
    REINIT(f1); REINIT(f2);
    f1->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_int_range(f1, PA_PROP_FORMAT_RATE, 32000, 48000);
    f2->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_int(f2, PA_PROP_FORMAT_RATE, 44100);
    pa_assert(pa_format_info_is_compatible(f1, f2));
    pa_assert(pa_format_info_is_compatible(f2, f1));

    /* 5. Check int range - negative */
    REINIT(f2);
    f2->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_int(f2, PA_PROP_FORMAT_RATE, 96000);
    pa_assert(!pa_format_info_is_compatible(f1, f2));
    pa_assert(!pa_format_info_is_compatible(f2, f1));

    /* 6. Simple fixed format string check */
    REINIT(f1); REINIT(f2);
    f1->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_string(f1, "format.test_string", "thing1");
    f2->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_string(f2, "format.test_string", "notthing1");
    pa_assert(!pa_format_info_is_compatible(f1, f2));

    /* 7. Check string array membership - positive */
    REINIT(f1); REINIT(f2);
    f1->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_string_array(f1, "format.test_string", strings, PA_ELEMENTSOF(strings));
    f2->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_string(f2, "format.test_string", "thing3");
    pa_assert(pa_format_info_is_compatible(f1, f2));
    pa_assert(pa_format_info_is_compatible(f2, f1));

    /* 8. Check string array membership - negative */
    REINIT(f2);
    f2->encoding = PA_ENCODING_AC3_IEC61937;
    pa_format_info_set_prop_string(f2, "format.test_string", "thing5");
    pa_assert(!pa_format_info_is_compatible(f1, f2));
    pa_assert(!pa_format_info_is_compatible(f2, f1));

    /* 9. Verify setting/getting an int */
    REINIT(f1);
    pa_format_info_set_prop_int(f1, "format.test_string", 42);
    pa_assert(pa_format_info_get_prop_type(f1, "format.test_string") == PA_PROP_TYPE_INT);
    pa_assert(pa_format_info_get_prop_int(f1, "format.test_string", &temp_int1) == 0);
    pa_assert(temp_int1 == 42);

    /* 10. Verify setting/getting an int range */
    REINIT(f1);
    pa_format_info_set_prop_int_range(f1, "format.test_string", 0, 100);
    pa_assert(pa_format_info_get_prop_type(f1, "format.test_string") == PA_PROP_TYPE_INT_RANGE);
    pa_assert(pa_format_info_get_prop_int_range(f1, "format.test_string", &temp_int1, &temp_int2) == 0);
    pa_assert(temp_int1 == 0 && temp_int2 == 100);

    /* 11. Verify setting/getting an int array */
    REINIT(f1);
    pa_format_info_set_prop_int_array(f1, "format.test_string", rates1, PA_ELEMENTSOF(rates1));
    pa_assert(pa_format_info_get_prop_type(f1, "format.test_string") == PA_PROP_TYPE_INT_ARRAY);
    pa_assert(pa_format_info_get_prop_int_array(f1, "format.test_string", &temp_int_array, &temp_int1) == 0);
    pa_assert(temp_int1 == PA_ELEMENTSOF(rates1));
    for (i = 0; i < temp_int1; i++)
        pa_assert(temp_int_array[i] == rates1[i]);
    pa_xfree(temp_int_array);

    /* 12. Verify setting/getting a string */
    REINIT(f1);
    pa_format_info_set_prop_string(f1, "format.test_string", "foo");
    pa_assert(pa_format_info_get_prop_type(f1, "format.test_string") == PA_PROP_TYPE_STRING);
    pa_assert(pa_format_info_get_prop_string(f1, "format.test_string", &temp_str) == 0);
    pa_assert(pa_streq(temp_str, "foo"));
    pa_xfree(temp_str);

    /* 13. Verify setting/getting an int array */
    REINIT(f1);
    pa_format_info_set_prop_string_array(f1, "format.test_string", strings, PA_ELEMENTSOF(strings));
    pa_assert(pa_format_info_get_prop_type(f1, "format.test_string") == PA_PROP_TYPE_STRING_ARRAY);
    pa_assert(pa_format_info_get_prop_string_array(f1, "format.test_string", &temp_str_array, &temp_int1) == 0);
    pa_assert(temp_int1 == PA_ELEMENTSOF(strings));
    for (i = 0; i < temp_int1; i++)
        pa_assert(pa_streq(temp_str_array[i], strings[i]));
    pa_format_info_free_string_array(temp_str_array, temp_int1);

    DEINIT(f1);
    DEINIT(f2);

    return 0;
}
