/* F2: nn.oe / nn.err / nn.aoc / nn.Nn + run-time configuration files of the nn layer.
 *
 * Configuration (all optional, read once from the app's external files dir):
 *   f2_lang.txt      language code the "console" reports: en fr pl ru it es de (the game reads the first two letters
 *                    in StringTableManager.LoadLanguages -> Language.GetLangNameFromCode).  Default: ru.
 *   f2_platform.txt  "switch" (default) or "android": what UnityEngine.Application.platform tells the game code. */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "f2_nn.h"

static char s_lang[16] = "ru";
static int s_switch_platform = 1;

static int read_word(const char *name, char *out, size_t n)
{
    char path[1024];
    const char *dir = f2_files_dir();
    if (!dir) return 0;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[64] = "";
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    size_t k = 0;
    for (const char *p = line; *p && k + 1 < n; p++)
        if (isalnum((unsigned char)*p) || *p == '-') out[k++] = *p;
        else if (k) break;
    out[k] = 0;
    return k > 0;
}

void f2_nn_set_language(const char *code)
{
    if (code && *code) { strncpy(s_lang, code, sizeof s_lang - 1); s_lang[sizeof s_lang - 1] = 0; }
}

const char *f2_nn_language(void) { return s_lang; }
int f2_nn_report_switch_platform(void) { return s_switch_platform; }

void f2_nn_config_load(void)
{
    char w[32];
    if (read_word("f2_lang.txt", w, sizeof w)) f2_nn_set_language(w);
    if (read_word("f2_platform.txt", w, sizeof w)) s_switch_platform = strcmp(w, "android") != 0;
    f2_log("F2: nn config: language \"%s\" (f2_lang.txt), Application.platform reported as %s (f2_platform.txt)",
           s_lang, s_switch_platform ? "Switch (32)" : "Android (11, the engine's own value)");
}

/* nn::oe::GetDesiredLanguage() -> nn::settings::LanguageCode = char[8] returned in x0 */
uint64_t nn_oe_GetDesiredLanguage(void)
{
    uint64_t v = 0;
    memcpy(&v, s_lang, strnlen(s_lang, 7));
    F2_ONCE("F2: nn oe.Language.GetDesired -> \"%s\"", s_lang);
    return v;
}

/* ---- nn.err ---- */
void nn_err_ShowError(uint32_t result)
{
    f2_log("F2: nn err.Error.Show(result 2%03u-%04u, raw 0x%x) -- the game wanted to show a system error dialog",
           result & 0x1ff, (result >> 9) & 0x1fff, result);
}
void nn_err_ShowErrorCode(uint64_t code)
{
    f2_log("F2: nn err.Error.Show(error code %u-%04u) -- the game wanted to show a system error dialog",
           (unsigned)(code & 0xffffffffu), (unsigned)(code >> 32));
}
void nn_err_ShowUnacceptableApplicationVersion(void) { f2_log("F2: nn err.Error.ShowUnacceptableApplicationVersion"); }

/* ---- nn.aoc: the Complete Edition data carries its expansions in the base RomFS; the game code never asks ---- */
int32_t nn_aoc_CountAddOnContent(void) { F2_ONCE("F2: nn aoc.CountAddOnContent -> 0"); return 0; }
int32_t nn_aoc_ListAddOnContent(int32_t *out, int32_t offset, int32_t count)
{
    (void)out; (void)offset; (void)count;
    F2_ONCE("F2: nn aoc.ListAddOnContent -> 0");
    return 0;
}

/* ---- nn.Nn.Abort(message): the SDK would terminate the application here ---- */
void nn_Abort(const char *message)
{
    f2_log("F2: FATAL-ish: nn.Nn.Abort(\"%s\") -- the Switch SDK aborts here; F2 logs and lets the caller continue", message ? message : "");
}
