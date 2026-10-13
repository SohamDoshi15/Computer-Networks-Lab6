#ifndef CN_CLI_H
#define CN_CLI_H

#include <stdint.h>

int cn_parse_port(const char *text, uint16_t *port_out);
int cn_parse_probability(const char *text, double *probability_out);
int cn_parse_positive_u32(const char *text, uint32_t *value_out);

#endif
