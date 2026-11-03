#ifndef SQUEEZE_H
#define SQUEEZE_H

#include <stdint.h>

struct exec_hdr;

/* The caller validates destination ranges against a_text and a_data. */
int     load_squeezed_segments(int fd, const struct exec_hdr *hdr,
                               uint8_t *text, uint8_t *data);

#endif
