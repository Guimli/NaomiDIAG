/* =============================================================================
 * strings.h -- compile-time localized user-facing strings.
 *
 * One language per ROM, picked at build time: make LANG=XX defines LANG_XX
 * and LANG_STRINGS="lang/xx.h", which this file includes (English, en.h, is
 * the base and the default). Every language file defines the same S_* keys.
 * Strings are UTF-8: the screen draws any character the language's glyph
 * table (src/lang/glyphs_xx.h, tools/gen_glyphs.py) holds.
 *
 * Only operator-facing narrative + result labels are localized. Low-level
 * technical fragments (hex register dumps, "crc=", per-address fail dumps)
 * stay in English on purpose: they are diagnostic detail read by a
 * technician and are language-neutral.
 *
 * Also included by crt0.S (macros are plain string literals, cpp-safe).
 * ========================================================================== */
#ifndef STRINGS_H
#define STRINGS_H
#include "config.h"           /* SCIF_BAUD_STR */

#ifdef LANG_STRINGS
#include LANG_STRINGS             /* "lang/xx.h", from the Makefile */
#else
#include "lang/en.h"
#endif

#endif /* STRINGS_H */
