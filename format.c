#include "format.h"

/* 9d08h
1h13m */

#define array_len(a) (sizeof(a) / sizeof(*a))

static char const TIME_UNITS[]	  = { 's', 'm', 'h', 'd', 'w' , '>' };
static uint32_t const TIME_BASE[] = {	1,	60,  60 * 60, 24 * 60 * 60, 7 * 24 * 60 * 60, 52 * 7 * 24 * 60 * 60, UINT32_MAX };

/* 29d24h19m30s */
int
fmt_time(char *str, uint32_t time) {
	uint8_t i;
	uint8_t t;

	for (i = 0; time > TIME_BASE[i + 1]; ++i)
		;

	if (array_len(TIME_BASE) - 2 <= i) {
		str[0] = 'n';
		str[1] = 'e';
		str[2] = 'v';
		str[3] = 'e';
		str[4] = 'r';
		return 5;
	}

	t = time / TIME_BASE[i];
	if (i > 0 && t < 10) {
		str[0] = (i < array_len(TIME_UNITS) ? '0' + t : ' ');
		str[1] = TIME_UNITS[i];
		time = (time - t * TIME_BASE[i]) / TIME_BASE[i - 1];
		--i;
	} else {
		str[0] = ' ';
		str[1] = ' ';
		time = t;
	}
	str[2] = '0' + (time / 10);
	str[3] = '0' + (time % 10);
	str[4] = TIME_UNITS[i];

	if (str[1] == ' ' && str[2] == '0')
		str[2] = ' ';
	return 5;
}

static int
fmt_decimal(char *str, uint64_t n, char const *UNITS, uint64_t const *BASE) {
	int i = 0;

	while (n >= 1000 * BASE[i++])
		;

	n *= BASE[2];
	n /= (BASE - 1)[i];

	if (' ' == (UNITS - 1)[i])
		*str++ = ' ';

	if (n >= 10 * BASE[2] || 1 == (BASE - 1)[i]) {
		n /= BASE[2];
		str[0] = n >= 100U ? '0' +  (n / 100U)        : ' ';
		str[1] = n >= 10U  ? '0' + ((n % 100U) / 10U) : ' ';
		str[2] = '0' + (n % 10U);
	} else {
		n /= BASE[2] / 10U;
		str[0] = '0' + (n / 10U);
		str[1] = '.';
		str[2] = '0' + (n % 10U);
	}

	if (' ' != (UNITS - 1)[i])
		str[3] = (UNITS - 1)[i];

	return 4;
}

static char const BYTES_METRIC_UNITS[] = { ' ', 'B', 'k', 'M', 'G', 'T', 'Y' };
static uint64_t const BYTES_METRIC_BASE[] = { 1, 1e0, 1e3, 1e6, 1e9, 1e12, 1e15, 1e18 };

int
fmt_speed(char *str, uint64_t num) {
	return fmt_decimal(str, num, BYTES_METRIC_UNITS, BYTES_METRIC_BASE);
}

#define E(n) ((uint64_t)1 << (10U * n))
static char const BYTES_IEC_UNITS[] = { ' ', 'B', 'K', 'M', 'G', 'T', 'Y' };
static uint64_t const BYTES_IEC_BASE[] = { 1, E(0), E(1), E(2), E(3), E(4), E(5), E(6) };
#undef E

int
fmt_space(char *str, uint64_t num) {
	return fmt_decimal(str, num, BYTES_IEC_UNITS, BYTES_IEC_BASE);
}

static char const NUMBER_UNITS[] = { ' ', ' ', 'k', 'm', 'b' };
static uint64_t const NUMBER_BASE[] = { 1, 1e0, 1e3, 1e6, 1e9, 1e12 };

int
fmt_number(char *str, uint64_t num) {
	return fmt_decimal(str, num, NUMBER_UNITS, NUMBER_BASE);
}

int fmt_percent(char *str, uint64_t num, uint64_t total) {
#define PERCENT * 100
	uint32_t p = total > 0 ? num * (uint64_t)(100 PERCENT) / total : 0;
	if (p < 10 PERCENT) {
		str[0] = '0' + (p / 100);
		str[1] = '.';
		str[2] = '0' + (p / 10 % 10);
		str[3] = '0' + (p % 10);
		str[4] = '%';
	} else if (p < 100 PERCENT) {
		p /= 10; /* Drop one decimal. */
		str[0] = '0' + (p / 100);
		str[1] = '0' + (p / 10 % 10);
		str[2] = '.';
		str[3] = '0' + (p % 10);
		str[4] = '%';
	} else if (p < 10000 PERCENT) {
		p /= 100; /* Drop two decimals. */
		str[0] = p < 1000 ? ' ' : '0' + (p / 1000 % 10);
		str[1] = '0' + (p / 100 % 10);
		str[2] = '0' + (p / 10 % 10);
		str[3] = '0' + (p % 10);
		str[4] = '%';
	} else {
		p /= 100 PERCENT;
		str[0] = 'x';
		return 1 + fmt_number(str + 1, p);
	}
#undef PERCENT
	return 5;
}
