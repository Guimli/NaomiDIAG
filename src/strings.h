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

/* How a language reads 20..63 out of its number clips (say_number):
 * NUM_UNIT_FIRST 1 says the unit before the tens ("ein und zwanzig");
 * NUM_JOIN says the joining clip w_and between the two (NUM_JOIN_ALL), only
 * French's "et un" for one (NUM_JOIN_ONE), or nothing (NUM_JOIN_NONE). */
#define NUM_JOIN_NONE 0
#define NUM_JOIN_ALL  1
#define NUM_JOIN_ONE  2
#ifndef NUM_UNIT_FIRST
#define NUM_UNIT_FIRST 0
#endif
#ifndef NUM_JOIN
#define NUM_JOIN NUM_JOIN_NONE
#endif

#endif /* STRINGS_H */
