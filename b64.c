#include <stdlib.h>

#include "b64.h"

char *
b64_enc(const unsigned char *str, size_t n, size_t *outn)
{
	static char const BASE64_CHARS[64] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"0123456789+/";

	size_t i, j;
	char *out = malloc((*outn = ((n + 2) / 3) * 4));
	if (out == NULL)
		return NULL;

	for (i = 0, j = 0; i + 3 <= n; i += 3, j += 4) {
		out[j + 0] = BASE64_CHARS[str[i + 0] >> 2];
		out[j + 1] = BASE64_CHARS[((str[i + 0] & 0x3U) << 4) | (str[i + 1] >> 4)];
		out[j + 2] = BASE64_CHARS[((str[i + 1] & 0xfU) << 2) | (str[i + 2] >> 6)];
		out[j + 3] = BASE64_CHARS[str[i + 2] & 0x3fU];
	}

	switch (n - i) {
	default:
#if defined(__GNUC__) || defined(__clang__)
		__builtin_unreachable();
#endif
	case 0:
		break;
	case 1:
		out[j + 0] = BASE64_CHARS[str[i + 0] >> 2];
		out[j + 1] = BASE64_CHARS[(str[i + 0] & 0x3) << 4];
		out[j + 2] = '=';
		out[j + 3] = '=';
		j += 4;
		break;
	case 2:
		out[j + 0] = BASE64_CHARS[str[i] >> 2];
		out[j + 1] = BASE64_CHARS[((str[i] & 0x3) << 4) | (str[i + 1] >> 4)];
		out[j + 2] = BASE64_CHARS[(str[i + 1] & 0xf) << 2];
		out[j + 3] = '=';
		j += 4;
		break;
	}

	return out;
}
