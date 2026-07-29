/* gl_support.c - the non-Skia half of scriptc-game's sg_ shim.
 *
 * WHY THIS EXISTS. scriptc-game's math FFI (sg_sqrt, sg_tan, ...) and its
 * error mailbox live in shim/sg_skia_extra.cpp, alongside the Skia canvas
 * helpers. A GL cart links no Skia, so excluding that file dropped the math
 * with it -- and `-sERROR_ON_UNDEFINED_SYMBOLS=0` turned every one of them
 * into a host-stubbed import returning a constant.
 *
 * The symptom was not a crash. `Math.tan(x)`, `Math.sqrt(x)` and
 * `Math.cos(x)` all returned the SAME wrong number regardless of argument,
 * so every camera matrix was quietly garbage, the renderer culled or
 * discarded the only mesh, and the frame came out empty with no error
 * anywhere. Two days of that trail led through struct layouts and asyncify
 * before landing on a missing object file.
 *
 * These are the same one-line wrappers as sg_skia_extra.cpp, minus
 * everything that touches Skia. The canvas cart keeps using the real file.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

/* ---- math (host/math.ts -> these) ---------------------------------- */

double sg_sqrt(double x)  { return sqrt(x); }
double sg_sin(double x)   { return sin(x); }
double sg_cos(double x)   { return cos(x); }
double sg_tan(double x)   { return tan(x); }
double sg_asin(double x)  { return asin(x); }
double sg_acos(double x)  { return acos(x); }
double sg_atan(double x)  { return atan(x); }
double sg_atan2(double y, double x) { return atan2(y, x); }
double sg_pow(double x, double y)   { return pow(x, y); }
double sg_exp(double x)   { return exp(x); }
double sg_log(double x)   { return log(x); }
double sg_log2(double x)  { return log2(x); }
double sg_log10(double x) { return log10(x); }
double sg_hypot(double x, double y) { return hypot(x, y); }
double sg_fmod(double x, double y)  { return fmod(x, y); }

/* ---- error mailbox --------------------------------------------------
 *
 * host/mailbox.ts reads a failure as a string through these. The GL shim
 * calls sg_mail_set on every error path, so a missing definition would turn
 * a real GL error into a stubbed no-op and, again, a silent blank frame. */

static char   g_mail[256];
static size_t g_mail_len;

void sg_mail_set(const char* s) {
    if (!s) { g_mail_len = 0; return; }
    size_t n = strlen(s);
    if (n >= sizeof(g_mail)) n = sizeof(g_mail) - 1;
    memcpy(g_mail, s, n);
    g_mail[n] = 0;
    g_mail_len = n;
}

uint32_t sg_str_len(int32_t unused)  { (void)unused; return (uint32_t)g_mail_len; }
uint32_t sg_str_byte(uint32_t i) {
    return i < g_mail_len ? (uint32_t)(uint8_t)g_mail[i] : 0u;
}
