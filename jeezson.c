/**
 * jeezson - JSON parser and generator
 * Copyright (C) 2020-2021  zsugabubus
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
# include <xlocale.h>
#endif

#include "jeezson.h"

#if __STDC_VERSION__ < 201112L
# define U(field) u.field
#else
# define U(field) field
#endif

enum { JSON_DEPTH_MAX = sizeof(size_t) * CHAR_BIT };

unsigned json_dump_max_level = UINT_MAX;

#if defined(__GNUC__) || defined(__clang__)
# define attribute_nonnull __attribute__((nonnull))
# define attribute_returnsnonnull __attribute__((returns_nonnull))
# define likely(x) __builtin_expect(!!(x), 1)
# define unlikely(x) likely(!x)
#else
# define attribute_nonnull
# define attribute_returnsnonnull
#endif

#if defined(__GNUC__)
# define attribute_alwaysinline __attribute__((always_inline))
# define attribute_const __attribute__((const))
#else
# define attribute_alwaysinline
# define attribute_const
#endif

typedef uint32_t char32_t;
typedef uint16_t char16_t;

attribute_alwaysinline
static __inline__ uint8_t
utf8_chrlen(char s)
{
	/*
	 * Prefix               Number of bytes
	 * --------------------------------
	 * 0???xxxx  0 - 7      1
	 * 110?xxxx  12-13      2
	 * 1110xxxx  14-14      3
	 * 1111xxxx  15-15      4
	 *
	 * */
#define B(v, n) ((v - 1) << (n * 2))
	return (((uint32_t)(B(4, 15) | B(3, 14) | B(2, 13) | B(2, 12)) >>
			((((unsigned char)s) >> 4) << 1)) &
		   3);
#undef B
}

attribute_nonnull
static __inline__ uint8_t
utf8_chrcpy(char *dest, char const *src)
{
	uint8_t const len = utf8_chrlen(*src);

	/* TODO: Measure against `rep mov'. */
	switch (len) {
	case 3:
		dest[len - 3] = src[len - 3]; /* fall through */
	case 2:
		dest[len - 2] = src[len - 2]; /* fall through */
	case 1:
		dest[len - 1] = src[len - 1]; /* fall through */
	default:
		dest[len] = src[len];
	}
	return len + 1;
}

attribute_nonnull
static uint8_t
utf32_toutf8(char *__restrict dest, char32_t codepoint)
{
	if (codepoint <= 0x7f) {
		/* U+0000..U+007f */
		dest[0] = codepoint;
		return 1;
	} else if (codepoint <= 0x7ff) {
		/* U+0080..U+07FF */
		dest[0] = 0xc0 | codepoint >> 6;
		dest[1] = 0x80 | (codepoint & 0x3f);
		return 2;
	} else if (codepoint <= 0xffff) {
		/* U+0800..U+FFFF */
		dest[0] = 0xe0 | codepoint >> 12;
		dest[1] = 0x80 | (codepoint >> 6 & 0x3f);
		dest[2] = 0x80 | (codepoint & 0x3f);
		return 3;
	} else {
		/* U+10000..U+10ffff */
		dest[0] = 0xf0 | codepoint >> 18;
		dest[1] = 0x80 | (codepoint >> 12 & 0x3f);
		dest[2] = 0x80 | (codepoint >> 6 & 0x3f);
		dest[3] = 0x80 | (codepoint & 0x3f);
		return 4;
	}
}

attribute_const static char32_t
utf32_fromsurrogates(char16_t high, char16_t low)
{
	return 0x10000U + (((char32_t)(high & 0x03ffU) << 10) | (low & 0x03ffU));
}

attribute_const attribute_alwaysinline
static __inline__ int
ascii_iscntrl(char c)
{
	return (unsigned char)c <= 0x1fU || (unsigned char)c == 0x7fU /* DEL */;
}

/**
 * @return whether character should be '\' escaped inside a string.
 */
attribute_const attribute_alwaysinline
static __inline__ int
json_isspecial(char c)
{
	return '"' == c || '\\' == c ||
	       /* So it can be embedded inside HTML. */
	       '/' == c;
}

static uint8_t const HEX_LOOKUP[16 + 10] = {
	0, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9
};

static char const *const HEX_DIGITS = "0123456789abcdef";

attribute_nonnull attribute_alwaysinline
static __inline__ uint8_t
hex8_fromstr(char const *__restrict src)
{
	return
		((HEX_LOOKUP[src[0] % 32]) << 4) |
		((HEX_LOOKUP[src[1] % 32]) << 0);
}

attribute_nonnull attribute_alwaysinline
static __inline__ uint16_t
hex16_fromstr(char const *__restrict src)
{
	return
		((HEX_LOOKUP[src[0] % 32]) << 12) |
		((HEX_LOOKUP[src[1] % 32]) << 8) |
		((HEX_LOOKUP[src[2] % 32]) << 4) |
		((HEX_LOOKUP[src[3] % 32]) << 0);
}

attribute_nonnull attribute_alwaysinline
static __inline__ void
hex16_tostr(char *dest, uint16_t val)
{
	dest[0] = HEX_DIGITS[(val >> 12) % 16];
	dest[1] = HEX_DIGITS[(val >>  8) % 16];
	dest[2] = HEX_DIGITS[(val >>  4) % 16];
	dest[3] = HEX_DIGITS[ val        % 16];
}

attribute_const attribute_nonnull
JSONNode *
json_get(JSONNode const *__restrict node, char const *__restrict key)
{
	size_t key_size;

	if (!json_length(node))
		return NULL;

	key_size = strlen(key) + 1;

	node = json_children(node);
	do
		if (!memcmp(node->key, key, key_size))
			break;
	while ((node = json_next(node)));

	return (JSONNode *)node;
}

attribute_nonnull attribute_returnsnonnull
static char *
json_parse_str(char *__restrict s)
{
	char *p = ++s;

	for (;;) {
		size_t n = strcspn(s, "\\\"");
		memmove(p, s, n);
		p += n, s += n;
		if (*s == '\\') {
			switch (s[1]) {
			default:
				p[0] = s[1];
				break;

			case 'b':
				p[0] = '\b';
				break;

			case 'f':
				p[0] = '\f';
				break;

			case 'n':
				p[0] = '\n';
				break;

			case 'r':
				p[0] = '\r';
				break;

			case 't':
				p[0] = '\t';
				break;

			case 'x':
			{
				s += 2;
				*p = hex8_fromstr(s);
				p += 1, s += 2;
			}
				continue;

			case 'u':
			{
				char16_t high, low;
				char32_t unicode;

				s += 2;
				high = hex16_fromstr(s);
				s += 4;

				if (high < 0xd800 || 0xdfff < high) {
					unicode = high;
				} else {
					/* UTF-16 surrogate pair. */
					s += 2;
					low = hex16_fromstr(s);
					s += 4;

					unicode = utf32_fromsurrogates(high, low);
				}

				p += utf32_toutf8(p, unicode);
			}
				continue;
			}
			p += 1, s += 2;
		} else {
			*p = '\0';
			return s + 1;
		}
	}
}

attribute_nonnull
size_t
json_parse(char *buf, JSONNode *__restrict *__restrict pnodes, size_t *__restrict pnb_nodes, unsigned flags)
{
	static locale_t cloc = (locale_t)0;

	char *s = buf;
	uint8_t depth = 0;
	size_t index = 0;
	size_t is_object = 0;
	size_t parents[JSON_DEPTH_MAX];
	size_t lengths[JSON_DEPTH_MAX];
	size_t siblings[JSON_DEPTH_MAX];
	locale_t origloc = (locale_t)0;

	JSONNode *nodes = *pnodes;
	size_t nb_nodes = *pnb_nodes;

	if ((locale_t)0 == cloc)
		cloc = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);

	for (;; ++index) {
		JSONNode *node;

		if (nb_nodes <= index) {
			nb_nodes = (nb_nodes * 8 / 5 /* Golden Ratio. */) + 128;

			if (!(nodes = realloc(nodes, nb_nodes * sizeof **pnodes))) {
				uselocale(origloc);
				return 0;
			}

			*pnodes = nodes;
			*pnb_nodes = nb_nodes;
		}

		node = &nodes[index];
		node->key = NULL;

	parse_token:
		switch (*s) {
		case ' ':
		case '\t':
		case '\r':
		case '\n':
			++s;
			goto parse_token;

		case '"':
		{
			int const is_value = !(is_object & ((size_t)1 << depth)) || node->key;
			if (is_value) {
				node->sibltype = JSONT_STRING;
				node->U(str) = s + 1;
			} else {
				node->key = s + 1;
			}

			s = json_parse_str(s);

			/* >> (depth + 1); underflowed? */
			if (is_value) {
				break;
			} else {
				while (':' != *s++);
				goto parse_token;
			}
		}

		case '[':
		case '{':
			if ('{' == *s) {
				node->sibltype = JSONT_OBJECT;
				is_object ^= (2 << depth);
			} else {
				node->sibltype = JSONT_ARRAY;
			}

			++depth;
			lengths[depth] = 0;
			parents[depth] = index;
			siblings[depth] = index + 1;
			s += 1;
			continue;

		case '}':
			is_object ^= (1 << depth);
			/* FALL THROUGH */
		case ']':
			nodes[parents[depth]].U(len) = lengths[depth] + !!(parents[depth] != index - 1);
			s += 1;
			if (!--depth)
				break;
			goto parse_token;

		case 'f':
			node->sibltype = JSONT_FALSE;
			s += strlen("false");
			break;

		case 't':
			node->sibltype = JSONT_TRUE;
			s += strlen("true");
			break;

		case ',':
			++lengths[depth];

			nodes[siblings[depth]].sibltype |= (index - siblings[depth]) << 3;
			siblings[depth] = index;

			s += 1;
			goto parse_token;

		case 'n':
			node->sibltype = JSONT_NULL;
			s += strlen("null");
			break;

		default:
			if (!(flags & JSON_FLAG_NO_NUMBERS)) {
				if ((locale_t)0 == origloc)
					origloc = uselocale(cloc);
				node->sibltype = JSONT_NUMBER;
				node->U(num) = strtod(s, &s);
			} else {
				node->sibltype = JSONT_STRING;
				node->U(str) = s;
				while (++s, (('0' <= *s && *s <= '9') || '.' == *s || '-' == *s || 'e' == *s));
			}
			break;
		}

		if (!depth)
			break;
	}

	if ((locale_t)0 != origloc)
		(void)uselocale(origloc);
	return s - buf;
}

static int
json_writer_ensure_size(JSONWriter *__restrict w, size_t size)
{
	char *p;

	/* Reserve space for closing brackets and one more token. */
	size += JSON_DEPTH_MAX +
	        32 /* Longest possible token (number). */ +
	        1 /* An extra colon. */ +
	        1 /* Terminating NUL. */;
	if (size <= w->buf_alloced)
		return 1;

	if (!(p = realloc(w->buf, (w->buf_alloced = size)))) {
		if (0 <= w->status)
			w->status = -ENOMEM;
		return 0;
	}
	w->buf = p;

	return 1;
}

void
json_writer_init(JSONWriter *__restrict w)
{
	memset(w, 0, sizeof *w);
	json_writer_ensure_size(w, 0);
}

#define json_write_lit(lit) \
	do { \
		memcpy(w->buf + w->buf_size, lit, sizeof(lit) - 1); \
		w->buf_size += strlen(lit); \
	} while (0)

static void
json_write_comma(JSONWriter *__restrict w)
{
	if (w->open < w->buf_size)
		json_write_lit(",");
}

void
json_write_null(JSONWriter *__restrict w)
{
	if (!json_writer_ensure_size(w, w->buf_size))
		return;

	json_write_comma(w);
	json_write_lit("null");
}

void
json_write_bool(JSONWriter *__restrict w, int b)
{
	if (!json_writer_ensure_size(w, w->buf_size))
		return;

	json_write_comma(w);

	if (b)
		json_write_lit("true");
	else
		json_write_lit("false");
}

void
json_write_num(JSONWriter *__restrict w, double num)
{
	if (!json_writer_ensure_size(w, w->buf_size))
		return;

	json_write_comma(w);

	w->buf_size += (size_t)sprintf(w->buf + w->buf_size, "%.15g", num);
}

void
json_write_int(JSONWriter *__restrict w, long num)
{
	if (!json_writer_ensure_size(w, w->buf_size))
		return;

	json_write_comma(w);

	w->buf_size += (size_t)sprintf(w->buf + w->buf_size, "%ld", num);
}

void
json_write_beginarr(JSONWriter *__restrict w)
{
	json_write_comma(w);
	json_write_lit("[");
	w->open = w->buf_size;
}

void
json_write_endarr(JSONWriter *__restrict w)
{
	json_write_lit("]");
	w->open = 0;
}

void
json_write_beginobj(JSONWriter *__restrict w)
{
	json_write_comma(w);
	json_write_lit("{");
	w->open = w->buf_size;
}

void
json_write_endobj(JSONWriter *__restrict w)
{
	json_write_lit("}");
	w->open = 0;
}

void
json_write_key(JSONWriter *__restrict w, char const *__restrict s)
{
	json_write_str(w, s);
	json_write_lit(":");
	w->open = w->buf_size;
}

void
json_write_str(JSONWriter *__restrict w, char const *__restrict s)
{
	char *p;
	char const *q;
	size_t new_size;

	json_write_comma(w);

	new_size = w->buf_size + 1 /* '"' */ + 1 /* '"' */;
	for (q = s; *q; ++q)
		if (ascii_iscntrl(q[0]))
			switch (q[0]) {
			case '\b':
			case '\t':
			case '\n':
			case '\f':
			case '\r':
				new_size += 2;
				break;
			default:
				new_size += 6;
				break;
			}
		else if (json_isspecial(q[0]))
			new_size += 2;

	new_size += q - s;
	if (!json_writer_ensure_size(w, new_size))
		return;

	p = w->buf + w->buf_size;

	*p++ = '\"';
	while (*s) {
		if (!ascii_iscntrl(s[0])) {
			if (!json_isspecial(s[0])) {
				uint8_t const nbytes = utf8_chrcpy(p, s);
				p += nbytes, s += nbytes;
				continue;
			} else {
				p[0] = '\\';
				p[1] = s[0];
				p += 2;
			}
		} else {
			p[0] = '\\';
#define B(c) (1 << c)
			if ((B('\b') | B('\t') | B('\n') | B('\f') | B('\r')) &
				(1 << s[0])) {
				p[1] = "btn:)fr"[s[0] - '\b'];
				p += 2;
			} else {
				p[1] = 'u';
				p += 2;
				hex16_tostr(p, s[0]);
				p += 4;
			}
#undef B
		}

		s += 1;
	}

	*p++ = '\"';
	w->buf_size = p - w->buf;
}

void
json_write_value(JSONWriter *__restrict w, JSONNode const *value)
{
	enum JSONNodeType type = json_type(value);
	JSONNode *child __attribute__((unused));

	/* TODO: Get rid of recursion. */
	switch (type) {
	case JSONT_FALSE:
	case JSONT_TRUE:
		json_write_bool(w, JSONT_TRUE == type);
		break;

	case JSONT_NULL:
		json_write_null(w);
		break;

	case JSONT_STRING:
		json_write_str(w, value->U(str));
		break;

	case JSONT_NUMBER:
		json_write_num(w, value->U(num));
		break;

	case JSONT_ARRAY:
		json_write_beginarr(w);
		json_each(child, value)
			json_write_value(w, child);
		json_write_endarr(w);
		break;

	case JSONT_OBJECT:
		json_write_beginobj(w);
		json_each(child, value)
			json_write_key(w, child->key), json_write_value(w, child);
		json_write_endobj(w);
		break;
	}
}

static void
json_dump_internal(JSONNode const *node, unsigned level, FILE *stream)
{
	enum JSONNodeType type;

	if (!node) {
		fprintf(stream, "(null)\n");
		return;
	}

	type = json_type(node);

	fprintf(stream, "%*s", level, "");

	if (node->key)
		fprintf(stream, "%s=", node->key);

	switch (type) {
	case JSONT_OBJECT:
	case JSONT_ARRAY:
		fputc(JSONT_OBJECT == type ? '{' : '[', stream);
		if (json_length(node)) {
			if (level < json_dump_max_level) {
				fputc('\n', stream);
				node = json_children(node);
				do
					json_dump_internal(node, level + 1, stream);
				while ((node = json_next(node)));
				fprintf(stream, "%*s", level, "");
			} else {
				fprintf(stream, " %zu children ", json_length(node));
			}
		}
		fprintf(stream, "%c\n", JSONT_OBJECT == type ? '}' : ']');
		break;

	case JSONT_NUMBER:
	{
		int64_t const int_num = node->U(num);
		if (node->U(num) == int_num)
			fprintf(stream, "%"PRId64"\n", int_num);
		else
			fprintf(stream, "%g\n", node->U(num));
	}
		break;
	case JSONT_STRING:
		fprintf(stream, "\"%s\"\n", node->U(str));
		break;

	case JSONT_TRUE:
	case JSONT_FALSE:
		fprintf(stream, "%s\n", JSONT_TRUE == type ? "true" : "false");
		break;

	case JSONT_NULL:
		fprintf(stream, "null\n");
		break;

	default:
		abort();
	}
}

void
json_dump(JSONNode const *node, FILE *stream)
{
	json_dump_internal(node, 0, stream);
}

char *
json_get_string(JSONNode const *node)
{
	JSONWriter w[1];

	json_writer_init(w);
	json_write_value(w, node);

	if (w->status < 0) {
		json_writer_free(w);
		return NULL;
	}

	w->buf[w->buf_size] = '\0';

	return w->buf;
}

