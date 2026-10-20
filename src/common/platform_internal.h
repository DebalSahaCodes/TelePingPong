/* platform_internal.h - helpers each OS file provides to net_common.c only. */
#ifndef PLATFORM_INTERNAL_H
#define PLATFORM_INTERNAL_H

#include <stddef.h>

int  net_in_progress(void);                          /* last connect() is still in progress */
void err_text(int code, char *buf, size_t n);        /* text for a given OS error code */

#endif
