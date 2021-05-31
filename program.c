#include <malloc.h>
#include <stdarg.h>
#include <stddef.h>

char *error_msg;

void
show_error(char const *format, ...)
{
	free(error_msg);
	error_msg = NULL;

	if (!format)
		return;

	va_list ap;

	va_start(ap, format);
	int size = vsnprintf(NULL, 0, format, ap) + 1 /* NUL */;
	va_end(ap);

	if ((error_msg =  malloc(size))) {
		va_start(ap, format);
		vsprintf(error_msg, format, ap);
		va_end(ap);
	}

#if 0
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
#endif
}
