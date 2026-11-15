/* Character-set handling: everything inside the browser is UTF-8. */
#ifndef BH_CHARSET_H
#define BH_CHARSET_H

#include <stddef.h>
#include <stdint.h>

/* windows-1252 code points for bytes 0x80..0x9F */
extern const uint16_t cp1252_high[32];

/* Looks for <meta charset=...> / <meta http-equiv=content-type ...> in the first 2048 bytes.
 * Returns malloc'd lowercase name or NULL. */
char *charset_sniff_meta(const uint8_t *data, size_t len);

/* Converts data to UTF-8 given a charset label (may be NULL). Returns malloc'd NUL-terminated string. */
char *charset_to_utf8(const uint8_t *data, size_t len, const char *label, size_t *out_len);

#endif
