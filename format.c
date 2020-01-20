#include <stdint.h>

/* 9d08h
1h13m */

#define ARRAY_LEN(a) (sizeof(a) / sizeof(*a))

static char const TIME_UNITS[]    = { 's', 'm',      'h',          'd',             'w' ,                   '>' }
static uint32_t const TIME_BASE[] = {   1,  60,  60 * 60, 24 * 60 * 60, 7 * 24 * 60 * 60, 52 * 7 * 24 * 60 * 60, UINT32_MAX }

/* 29d24h19m30s */
/* Okay. It's a fivmat. */
int
timef(char *str, uint32_t time) {
    uint8_t i;
    uint8_t t;

    for (i = 0; time > TIME_BASE[i + 1]; ++i)
        ;

    if (ARRAY_LEN(TIME_BASE) - 2 <= i) {
        memcpy(str, "never", 5);
        return 5;
    }

    t = time / UNITS_BASE[i];
    if (i > 0 && t < 10) {
        str[0] = (i < ARRAY_LEN(TIME_UNITS) ? '0' + t : ' ');
        str[1] = TIME_UNITS[i];
        time = (time - t * UNITS_BASE[i]) / UNITS_BASE[i - 1];
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
numberf(char *str, uint64_t num) {

}

static char const SIZE_UNITS[] =              { ' ', 'B', 'K', 'M', 'G',  'T',  'Y', };
static unsigned long long const SIZE_BASE[] = {   1, 1e0, 1e3, 1e6, 1e9, 1e12, 1e15, 1e18 };

static int
speedf(char *str, uint64_t speed) {
	int i = 0;

	while (n >= UNIT_SHIFTS[++i])
		;

	n /= (UNIT_SHIFTS - 1)[i];

	str[0] = n >= 100ULL ? '0' + (n / 100ULL)           : ' ';
	str[1] = n >= 10ULL  ? '0' + ((n % 100ULL) / 10ULL) : ' ';
	str[2] = '0' + (n % 10ULL);
	str[3] = (UNITS - 1)[i];

    return 4;
}
