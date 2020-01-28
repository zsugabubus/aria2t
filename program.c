#include <stddef.h>
#include <string.h>

char const *program_name;

void set_program_name(char *argv0) {
	if (NULL != (program_name = strrchr(argv0, '/')))
		++program_name;
	else
		program_name = argv0;

	if (NULL == program_name || '\0' == *program_name)
		program_name = argv0;
}
