#include <stdlib.h>

#include "base64.h"

static char const BASE64_CHARS[64] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *b64_enc(const char *str, size_t n, size_t *outn) {
	size_t i, j;
	char *out = malloc((*outn = ((n + 2) / 3) * 4));
	if (out == NULL)
		return NULL;

	for (i = 0, j = 0; i + 3 <= n; i += 3, j += 4) {
		out[j + 0] = BASE64_CHARS[str[0] >> 2];
		out[j + 1] = BASE64_CHARS[((str[0] & 0x3) << 4) | (str[1] >> 4)];
		out[j + 2] = BASE64_CHARS[((str[1] & 0xf) << 2) | (str[2] >> 6)];
		out[j + 3] = BASE64_CHARS[str[2] & 0x3f];
	}

	switch (n - i) {
	default:
#if defined(__GNUC__) || defined(__clang__)
		__builtin_unreachable();
#endif
	case 0:
		break;
	case 1:
		out[j + 0] = BASE64_CHARS[str[0] >> 2];
		out[j + 1] = BASE64_CHARS[(str[0] & 0x3) << 4];
		out[j + 2] = '=';
		out[j + 3] = '=';
		j += 4;
		break;
	case 2:
		out[j + 0] = BASE64_CHARS[str[0] >> 2];
		out[j + 1] = BASE64_CHARS[((str[0] & 0x3) << 4) | (str[1] >> 4)];
		out[j + 2] = BASE64_CHARS[(str[1] & 0xf) << 2];
		out[j + 3] = '=';
		j += 4;
		break;
	}

	return out;
}
