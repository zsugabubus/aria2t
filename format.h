#ifndef FORMAT_H
#define FORMAT_H
#include <stdint.h>

int
fmt_time(char *str, uint32_t time);

int
fmt_speed(char *str, uint64_t num);

int
fmt_space(char *str, uint64_t num);

int
fmt_number(char *str, uint64_t num);

int
fmt_percent(char *str, uint64_t num, uint64_t total);
#endif
