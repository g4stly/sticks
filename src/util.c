#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

int random_int(int max)
{
	int rv;
	int divisor = RAND_MAX / (max + 1);
	do rv = rand() / divisor; while (rv > max);
	return rv;
}

void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (fmt[0] && fmt[strlen(fmt)-1] == ':') {
		fputc(' ', stderr);
		perror(NULL);
	}

	exit(1);
}
