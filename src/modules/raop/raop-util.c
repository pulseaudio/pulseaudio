/***
  This file is part of PulseAudio.

  Copyright 2008 Colin Guthrie
  Copyright Kungliga Tekniska högskolan
  Copyright 2013 Martin Blanchard

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

/***
  The base64 implementation was originally inspired by a file developed
  by Kungliga Tekniska högskolan.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/md5.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "raop-util.h"

#ifndef MD5_DIGEST_LENGTH
#define MD5_DIGEST_LENGTH 16
#endif

#define MD5_HASH_LENGTH (2*MD5_DIGEST_LENGTH)

#define BASE64_DECODE_ERROR 0xffffffff

static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int char_position(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 0;
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;

    return -1;
}

static unsigned int token_decode(const char *token) {
    unsigned int val = 0;
    int marker = 0;
    int i;

    if (strlen(token) < 4)
        return BASE64_DECODE_ERROR;
    for (i = 0; i < 4; i++) {
        val *= 64;
        if (token[i] == '=')
            marker++;
        else if (marker > 0)
            return BASE64_DECODE_ERROR;
        else {
            int lpos = char_position(token[i]);
            if (lpos < 0)
                return BASE64_DECODE_ERROR;
            val += lpos;
        }
    }

    if (marker > 2)
        return BASE64_DECODE_ERROR;

    return (marker << 24) | val;
}

int pa_raop_base64_encode(const void *data, int len, char **str) {
    const unsigned char *q;
    char *p, *s = NULL;
    int i, c;

    pa_assert(data);
    pa_assert(str);

    p = s = pa_xnew(char, len * 4 / 3 + 4);
    q = (const unsigned char *) data;
    for (i = 0; i < len;) {
        c = q[i++];
        c *= 256;
        if (i < len)
            c += q[i];
        i++;
        c *= 256;
        if (i < len)
            c += q[i];
        i++;
        p[0] = base64_chars[(c & 0x00fc0000) >> 18];
        p[1] = base64_chars[(c & 0x0003f000) >> 12];
        p[2] = base64_chars[(c & 0x00000fc0) >> 6];
        p[3] = base64_chars[(c & 0x0000003f) >> 0];
        if (i > len)
            p[3] = '=';
        if (i > len + 1)
            p[2] = '=';
        p += 4;
    }

    *p = 0;
    *str = s;
    return strlen(s);
}

int pa_raop_base64_decode(const char *str, void *data) {
    const char *p;
    unsigned char *q;

    pa_assert(str);
    pa_assert(data);

    q = data;
    for (p = str; *p && (*p == '=' || strchr(base64_chars, *p)); p += 4) {
        unsigned int val = token_decode(p);
        unsigned int marker = (val >> 24) & 0xff;
        if (val == BASE64_DECODE_ERROR)
            return -1;
        *q++ = (val >> 16) & 0xff;
        if (marker < 2)
            *q++ = (val >> 8) & 0xff;
        if (marker < 1)
            *q++ = val & 0xff;
    }

    return q - (unsigned char *) data;
}

int pa_raop_md5_hash(const char *data, int len, char **str) {
    unsigned char d[MD5_DIGEST_LENGTH];
    char *s = NULL;
    int i;

    pa_assert(data);
    pa_assert(str);

    MD5((unsigned char*) data, len, d);
    s = pa_xnew(char, MD5_HASH_LENGTH);
    for (i = 0; i < MD5_DIGEST_LENGTH; i++)
        sprintf(&s[2*i], "%02x", (unsigned int) d[i]);

    *str = s;
    s[MD5_HASH_LENGTH] = 0;
    return strlen(s);
}

int pa_raop_basic_response(const char *user, const char *pwd, char **str) {
    char *tmp, *B = NULL;

    pa_assert(str);

    tmp = pa_sprintf_malloc("%s:%s", user, pwd);
    pa_raop_base64_encode(tmp, strlen(tmp), &B);
    pa_xfree(tmp);

    *str = B;
    return strlen(B);
}

int pa_raop_digest_response(const char *user, const char *realm, const char *password,
                            const char *nonce, const char *uri, char **str) {
    char *A1, *HA1, *A2, *HA2;
    char *tmp, *KD = NULL;

    pa_assert(str);

    A1 = pa_sprintf_malloc("%s:%s:%s", user, realm, password);
    pa_raop_md5_hash(A1, strlen(A1), &HA1);
    pa_xfree(A1);

    A2 = pa_sprintf_malloc("OPTIONS:%s", uri);
    pa_raop_md5_hash(A2, strlen(A2), &HA2);
    pa_xfree(A2);

    tmp = pa_sprintf_malloc("%s:%s:%s", HA1, nonce, HA2);
    pa_raop_md5_hash(tmp, strlen(tmp), &KD);
    pa_xfree(tmp);

    pa_xfree(HA1);
    pa_xfree(HA2);

    *str = KD;
    return strlen(KD);
}
