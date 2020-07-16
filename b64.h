#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>

char *b64_enc(const unsigned char *str, size_t n, size_t *outn);

#endif
