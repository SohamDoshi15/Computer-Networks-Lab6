#include "cli.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

static int has_forbidden_prefix(const char *text)
{
    return text == NULL || text[0] == '\0' || text[0] == '+' || text[0] == '-' ||
           text[0] == ' ' || text[0] == '\t' || text[0] == '\n';
}

int cn_parse_port(const char *text, uint16_t *port_out)
{
    char *end = NULL;
    unsigned long value;

    if (port_out == NULL || has_forbidden_prefix(text)) {
        return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > 65535) {
        return -1;
    }
    *port_out = (uint16_t)value;
    return 0;
}

int cn_parse_probability(const char *text, double *probability_out)
{
    char *end = NULL;
    double value;

    if (probability_out == NULL || has_forbidden_prefix(text)) {
        return -1;
    }
    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value) ||
        value < 0.0 || value > 1.0) {
        return -1;
    }
    *probability_out = value;
    return 0;
}

int cn_parse_positive_u32(const char *text, uint32_t *value_out)
{
    char *end = NULL;
    unsigned long long value;

    if (value_out == NULL || has_forbidden_prefix(text)) {
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 ||
        value > UINT32_MAX) {
        return -1;
    }
    *value_out = (uint32_t)value;
    return 0;
}
