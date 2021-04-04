#include <malloc.h>
#include <stdarg.h>
#include <stddef.h>

char *error_msg;

void
set_error_msg(char const *format, ...)
{
	va_list argptr;
	int size;

	free(error_msg);
	error_msg = NULL;

	if (format) {
		va_start(argptr, format);
		size = vsnprintf(NULL, 0, format, argptr) + 1 /* NUL */;
		va_end(argptr);

		if ((error_msg =  malloc(size))) {
			va_start(argptr, format);
			vsprintf(error_msg, format, argptr);
			va_end(argptr);
		}
	}
}
