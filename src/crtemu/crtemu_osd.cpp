// crt-emu in-game panel for the cnc-ddraw OpenGL renderer - see inc/crtemu_osd.h.
//
// Threads: the panel is built and drawn on the render thread (owner of the GL context and of
// the ImGui context). Input arrives on the game's UI thread through a WH_GETMESSAGE hook that
// is installed when the panel is opened for the first time; while the panel is visible the hook
// hands mouse and keyboard messages to a small locked queue and turns them into WM_NULL, so
// neither the game nor other hooks installed earlier (HD mod) see them.
//
// Built without the C++ runtime: no exceptions, RTTI, STL or thread-safe statics.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_opengl3.h"
// the backend's own GL loader, for the few texture calls of the magnifier: every build of the
// panel (the DLL and the test hosts) links it with the backend, and it is loaded once the backend is
#include "backends/imgui_impl_opengl3_loader.h"
#include "crtemu_osd.h"

#ifdef CRTEMU_OSD_TEST
extern "C" { volatile long g_crtemu_shader_off = 0; }
#else
extern "C" volatile long g_crtemu_shader_off;   // render_ogl.c, keytoggleshader
#endif

extern "C" { volatile long g_crtemu_osd_visible = 0; }

// ---------------------------------------------------------------------------------------------
// parameters
// ---------------------------------------------------------------------------------------------

enum { MAXP = 64, MAXG = 12, MAXPRE = 16, MAXOPT = 12, PATHLEN = 520 };

struct Param
{
    char name[40];
    char label[160];
    char hint[768];
    char group[32];
    float def, min, max, step;      // #pragma parameter
    float value, saved;             // live value, value in the file
    int nopt;
    char opt[MAXOPT][96];
};

struct Group { char id[32]; char label[96]; };
struct Preset { char id[40]; char name[96]; char vals[1400]; };

static Param  P[MAXP];   static int NP;
static Group  G[MAXG];   static int NG;
static Preset PR[MAXPRE]; static int NPR;
static char   g_file[PATHLEN];
static volatile long g_loaded;          // parameters available -> panel may open
static char   g_status[300];
static int    g_status_ok;
static int    g_preset = -1;            // preset the current values came from, -1 = own

// ---------------------------------------------------------------------------------------------
// language: lang= in install.ini next to the shader (the panel's EN/RU buttons write it), else the
// game's language (HD mod profile), else the Windows display language, else English
// ---------------------------------------------------------------------------------------------

enum
{
    S_TITLE, S_OWN, S_OWN_DIRTY, S_NO_PRESETS, S_AB, S_SAVE, S_REVERT, S_UNSAVED, S_PARAMS, S_FOOTER,
    S_READ_FAIL, S_WRITE_FAIL, S_SAVED, S_DEFAULTS_ONLY, S_LANG_TIP, S_LANG_FAIL,
    S_LENS, S_LENS_ZOOM, S_LENS_SIZE, S_LENS_PX, S_LENS_TIP, S_COUNT
};

static const char* const LANG_ID[] = { "en", "ru" };
static const char* const LANG_BTN[] = { "EN", "RU" };
enum { NLANG = 2 };

static const char* const STR[S_COUNT][2] = {
    { "crt-emu · CRT tube###crtemu", "crt-emu · кинескоп###crtemu" },
    { "Custom settings", "Свои настройки" },
    { "Custom settings (unsaved)", "Свои настройки (не сохранены)" },
    { "No presets in the shader", "Пресетов в шейдере нет" },
    { "Before / after (F11)", "До / после (F11)" },
    { "Save", "Сохранить" },
    { "Revert to saved", "Вернуть сохранённое" },
    { "unsaved", "не сохранено" },
    { "Parameters", "Параметры" },
    { "F12 or Esc — close · Ctrl+click — type a value · right-click — back to the saved value",
      "F12 или Esc — закрыть · Ctrl+клик — ввести число · правый клик — вернуть сохранённое значение" },
    { "Could not read %s", "Не удалось прочитать %s" },
    { "Could not write the shader file (no write access to the game folder?)",
      "Не удалось записать файл шейдера (нет прав на папку игры?)" },
    { "Saved to %s%s", "Сохранено в %s%s" },
    { " (defaults only)", " (только значения по умолчанию)" },
    { "Panel language", "Язык панели" },
    { "Could not save the language to install.ini (no write access to the game folder?)",
      "Не удалось сохранить язык в install.ini (нет прав на папку игры?)" },
    { "Magnifier", "Лупа" },
    { "Zoom", "Увеличение" },
    { "Size", "Размер" },
    { "%d px", "%d пикс." },
    { "A close-up of the screen's own pixels: the mask and the lines at their real size. "
      "Drag the magnifier with the mouse, the wheel over it changes the zoom. "
      "It is shown only with the panel; Win+Shift+S takes a screenshot with it.",
      "Пиксели самого экрана вблизи: маска и строки в настоящем размере. "
      "Лупу двигайте мышью, колесо над ней меняет увеличение. "
      "Она видна только вместе с панелью; Win+Shift+S — снимок экрана вместе с ней." },
};

static int g_lang;                      // index into LANG_ID and STR

static const char* tr(int s) { return STR[s][g_lang]; }

static const char* skip_ws(const char* s) { while (*s == ' ' || *s == '\t') s++; return s; }

static int same_ci(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return 1;
}

static int field_is(const char* s, const char* e, const char* str)
{
    size_t n = strlen(str);
    return (size_t)(e - s) == n && strncmp(s, str, n) == 0;
}

// locale-independent number parsing / printing (the game may run with a decimal comma)
static int parse_num(const char* s, float* out, const char** end)
{
    const char* p = skip_ws(s);
    int neg = 0;
    if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
    if (!((*p >= '0' && *p <= '9') || (*p == '.' && p[1] >= '0' && p[1] <= '9'))) return 0;
    double v = 0.0;
    while (*p >= '0' && *p <= '9') v = v * 10.0 + (*p++ - '0');
    if (*p == '.')
    {
        p++;
        double f = 0.1;
        while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * f; f *= 0.1; }
    }
    if (*p == 'e' || *p == 'E')
    {
        const char* q = p + 1;
        int eneg = 0, e = 0;
        if (*q == '-' || *q == '+') { eneg = *q == '-'; q++; }
        if (*q >= '0' && *q <= '9')
        {
            while (*q >= '0' && *q <= '9') e = e * 10 + (*q++ - '0');
            v *= pow(10.0, eneg ? -e : e);
            p = q;
        }
    }
    *out = (float)(neg ? -v : v);
    if (end) *end = p;
    return 1;
}

static int decimals(const Param& p) { return p.step < 0.0099f ? 3 : 2; }

static void fmt_num(char* buf, size_t n, float v, int dec)
{
    double m = dec == 3 ? 1000.0 : 100.0;
    long long q = (long long)floor(fabs((double)v) * m + 0.5);
    long long ip = q / (long long)m, fp = q % (long long)m;
    snprintf(buf, n, "%s%lld.%0*lld", (v < 0 && q) ? "-" : "", ip, dec, fp);
}

static void copy_field(char* dst, size_t n, const char* s, const char* e)
{
    size_t len = (size_t)(e - s);
    if (len >= n) len = n - 1;
    memcpy(dst, s, len);
    dst[len] = 0;
}

static Param* find_param(const char* name, size_t len)
{
    for (int i = 0; i < NP; i++)
        if (strlen(P[i].name) == len && strncmp(P[i].name, name, len) == 0) return &P[i];
    return NULL;
}

static char* read_file(const char* path, long* size)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = n > 0 && n < 4 * 1024 * 1024 ? (char*)malloc(n + 1) : NULL;
    if (buf && fread(buf, 1, n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) { buf[n] = 0; if (size) *size = n; }
    return buf;
}

// iterate lines: returns pointer to the next line, sets [*ls, *le) without the line break
static const char* next_line(const char* p, const char** ls, const char** le)
{
    if (!*p) return NULL;
    *ls = p;
    while (*p && *p != '\n') p++;
    *le = (p > *ls && p[-1] == '\r') ? p - 1 : p;
    return *p ? p + 1 : p;
}

// "//@kind|a|b|c" -> fields split on '|', returns count
static int split_meta(const char* s, const char* e, const char* f[][2], int max)
{
    int n = 0;
    const char* a = s;
    for (const char* p = s; p <= e && n < max; p++)
    {
        if (p == e || *p == '|') { f[n][0] = a; f[n][1] = p; n++; a = p + 1; }
    }
    return n;
}

// "//@lang|xx" -> [*vs, *ve) = "xx"; 0 for any other line
static int lang_line(const char* s, const char* le, const char** vs, const char** ve)
{
    if (strncmp(s, "//@lang|", 8) != 0) return 0;
    *vs = s + 8;
    *ve = le;
    while (*ve > *vs && ((*ve)[-1] == ' ' || (*ve)[-1] == '\t')) (*ve)--;
    return 1;
}

static void parse_params(const char* text)
{
    NP = NG = NPR = 0;
    const char *ls, *le, *p;

    // metadata block to read: "//@lang|<panel language>", else "//@lang|en", else the first one.
    // Records before the first "//@lang" count for every language (shaders older than the blocks).
    char block[16] = "";
    int have_mine = 0, have_en = 0;
    for (p = text; (p = next_line(p, &ls, &le)) != NULL;)
    {
        const char *vs, *ve;
        if (!lang_line(skip_ws(ls), le, &vs, &ve)) continue;
        if (!block[0]) copy_field(block, sizeof(block), vs, ve);
        if (field_is(vs, ve, LANG_ID[g_lang])) have_mine = 1;
        if (field_is(vs, ve, "en")) have_en = 1;
    }
    if (have_mine) snprintf(block, sizeof(block), "%s", LANG_ID[g_lang]);
    else if (have_en) snprintf(block, sizeof(block), "en");
    int use = 1;

    // pass 1: #pragma parameter NAME "desc" def min max step
    for (p = text; (p = next_line(p, &ls, &le)) != NULL;)
    {
        const char* s = skip_ws(ls);
        if (strncmp(s, "#pragma parameter", 17) != 0 || NP >= MAXP) continue;
        s = skip_ws(s + 17);
        const char* ne = s;
        while (ne < le && *ne != ' ' && *ne != '\t') ne++;
        const char* q1 = (const char*)memchr(ne, '"', le - ne);
        const char* q2 = q1 ? (const char*)memchr(q1 + 1, '"', le - q1 - 1) : NULL;
        if (ne == s || !q2) continue;
        Param& pr = P[NP];
        memset(&pr, 0, sizeof(pr));
        copy_field(pr.name, sizeof(pr.name), s, ne);
        copy_field(pr.label, sizeof(pr.label), q1 + 1, q2);
        const char* t = q2 + 1;
        if (parse_num(t, &pr.def, &t) && parse_num(t, &pr.min, &t) &&
            parse_num(t, &pr.max, &t) && parse_num(t, &pr.step, &t) && !find_param(pr.name, strlen(pr.name)))
        {
            if (pr.step <= 0.0f) pr.step = 0.01f;
            pr.value = pr.saved = pr.def;
            NP++;
        }
    }

    // pass 2: #define NAME value, //@ metadata
    for (p = text; (p = next_line(p, &ls, &le)) != NULL;)
    {
        const char* s = skip_ws(ls);
        if (strncmp(s, "#define", 7) == 0)
        {
            s = skip_ws(s + 7);
            const char* ne = s;
            while (ne < le && *ne != ' ' && *ne != '\t') ne++;
            Param* pr = find_param(s, ne - s);
            float v;
            if (pr && parse_num(ne, &v, NULL)) pr->value = pr->saved = v;
        }
        else if (strncmp(s, "//@", 3) == 0)
        {
            const char *vs, *ve;
            if (lang_line(s, le, &vs, &ve)) { use = field_is(vs, ve, block); continue; }
            if (!use) continue;
            const char* f[MAXOPT + 2][2];
            int n = split_meta(s + 3, le, f, MAXOPT + 2);
            if (n < 2) continue;
            size_t kl = f[0][1] - f[0][0];
            if (kl == 5 && strncmp(f[0][0], "group", 5) == 0 && n >= 3 && NG < MAXG)
            {
                copy_field(G[NG].id, sizeof(G[NG].id), f[1][0], f[1][1]);
                copy_field(G[NG].label, sizeof(G[NG].label), f[2][0], f[2][1]);
                NG++;
            }
            else if (kl == 5 && strncmp(f[0][0], "param", 5) == 0 && n >= 4)
            {
                Param* pr = find_param(f[1][0], f[1][1] - f[1][0]);
                if (!pr) continue;
                copy_field(pr->group, sizeof(pr->group), f[2][0], f[2][1]);
                copy_field(pr->label, sizeof(pr->label), f[3][0], f[3][1]);
                if (n >= 5) copy_field(pr->hint, sizeof(pr->hint), f[4][0], f[4][1]);
            }
            else if (kl == 7 && strncmp(f[0][0], "options", 7) == 0 && n >= 3)
            {
                Param* pr = find_param(f[1][0], f[1][1] - f[1][0]);
                if (!pr) continue;
                pr->nopt = 0;
                for (int i = 2; i < n && pr->nopt < MAXOPT; i++, pr->nopt++)
                    copy_field(pr->opt[pr->nopt], sizeof(pr->opt[0]), f[i][0], f[i][1]);
            }
            else if (kl == 6 && strncmp(f[0][0], "preset", 6) == 0 && n >= 4 && NPR < MAXPRE)
            {
                copy_field(PR[NPR].id, sizeof(PR[NPR].id), f[1][0], f[1][1]);
                copy_field(PR[NPR].name, sizeof(PR[NPR].name), f[2][0], f[2][1]);
                copy_field(PR[NPR].vals, sizeof(PR[NPR].vals), f[3][0], f[3][1]);
                NPR++;
            }
        }
    }
}

static int match_preset(void);

static int file_exists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// "a\b\c" -> "a\b\", "a\b\" -> "a\", "a" -> ""
static void parent_dir(char* path)
{
    size_t n = strlen(path);
    if (n && (path[n - 1] == '\\' || path[n - 1] == '/')) n--;
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    path[n] = 0;
}

// install.ini in the folder of the shader file
static void ini_path(char* out, size_t n, const char* shader_file)
{
    char dir[PATHLEN];
    snprintf(dir, sizeof(dir), "%s", shader_file);
    parent_dir(dir);
    snprintf(out, n, "%sinstall.ini", dir);
}

// "key = value" (key in any case) -> [*vs, *ve) = the value without the spaces around it
static int ini_line(const char* ls, const char* le, const char* key, const char** vs, const char** ve)
{
    const char* s = skip_ws(ls);
    const char* eq = (const char*)memchr(s, '=', le - s);
    if (!eq) return 0;
    const char* ke = eq;
    while (ke > s && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    size_t kl = strlen(key);
    if ((size_t)(ke - s) != kl || !same_ci(s, key, kl)) return 0;
    *vs = skip_ws(eq + 1);
    *ve = le;
    while (*ve > *vs && ((*ve)[-1] == ' ' || (*ve)[-1] == '\t')) (*ve)--;
    return 1;
}

// "en" / "RU" -> index into LANG_ID, -1 for any other
static int lang_index(const char* s, const char* e)
{
    for (int i = 0; i < NLANG; i++)
        if ((size_t)(e - s) == strlen(LANG_ID[i]) && same_ci(s, LANG_ID[i], e - s)) return i;
    return -1;
}

// "lang = xx" in install.ini next to the shader; -1 if there is none or the language is unknown
static int read_lang(const char* shader_path)
{
    char path[PATHLEN];
    ini_path(path, sizeof(path), shader_path);
    char* text = read_file(path, NULL);
    if (!text) return -1;
    int lang = -1;
    const char *ls, *le, *p, *vs, *ve;
    for (p = text; (p = next_line(p, &ls, &le)) != NULL;)
    {
        if (!ini_line(ls, le, "lang", &vs, &ve)) continue;
        int l = lang_index(vs, ve);
        if (l >= 0) lang = l;
    }
    free(text);
    return lang;
}

// the game's language: <Language> = "#ru.ini" in the HD mod profile of HotA (only that one), two
// folders above the shader (Shaders\crt-emu\); -1 if there is no profile or its language is not ours
static int game_lang(const char* shader_path)
{
    char path[PATHLEN];
    snprintf(path, sizeof(path), "%s", shader_path);
    parent_dir(path);
    parent_dir(path);
    parent_dir(path);
    size_t n = strlen(path);
    snprintf(path + n, sizeof(path) - n, "_HD3_Data/Settings/hota.ini");
    char* text = read_file(path, NULL);
    if (!text) return -1;
    int lang = -1;
    const char *ls, *le, *p;
    for (p = text; (p = next_line(p, &ls, &le)) != NULL;)
    {
        const char* s = skip_ws(ls);
        if (le - s < 10 || strncmp(s, "<Language>", 10) != 0) continue;
        const char* v = (const char*)memchr(s, '#', le - s);
        if (v)
        {
            const char* e = ++v;
            while (e < le && *e != '.' && *e != '"') e++;
            lang = lang_index(v, e);
        }
        break;
    }
    free(text);
    return lang;
}

// the Windows display language; -1 elsewhere
static int system_lang(void)
{
#ifdef _WIN32
    static const char ru[] = "ru";
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? lang_index(ru, ru + 2) : 0;
#else
    return -1;
#endif
}

static int pick_lang(const char* shader_path)
{
    int lang = read_lang(shader_path);
    if (lang < 0) lang = game_lang(shader_path);
    if (lang < 0) lang = system_lang();
    return lang < 0 ? 0 : lang;
}

extern "C" void crtemu_osd_load(const char* shader_path)
{
    char path[PATHLEN];
    snprintf(path, sizeof(path), "%s.pass1", shader_path);
    if (!file_exists(path)) snprintf(path, sizeof(path), "%s", shader_path);

    if (g_loaded && strcmp(path, g_file) == 0) return;   // renderer restart: keep live values

    g_lang = pick_lang(shader_path);
    char* text = read_file(path, NULL);
    if (!text) { g_loaded = 0; NP = 0; return; }
    parse_params(text);
    free(text);
    snprintf(g_file, sizeof(g_file), "%s", path);
    g_preset = match_preset();
    g_status[0] = 0;
    g_loaded = NP > 0;
}

static int same_path(const char* a, const char* b)
{
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

extern "C" int crtemu_osd_is_param_file(const char* file_path)
{
    return g_loaded && file_path && same_path(file_path, g_file);
}

extern "C" int crtemu_osd_param_count(void) { return g_loaded ? NP : 0; }
extern "C" const char* crtemu_osd_param_name(int i) { return P[i].name; }
extern "C" float crtemu_osd_param_value(int i) { return P[i].value; }

// write through path.tmp, so a failed write leaves the old file whole
static int replace_file(const char* path, const char* data, size_t len)
{
    char tmp[PATHLEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    int ok = f && fwrite(data, 1, len, f) == len;
    if (f) ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) remove(tmp);
    return ok;
}

// rewrite the #define and #pragma parameter values in the shader file
static int save_params(void)
{
    long size = 0;
    char* text = read_file(g_file, &size);
    if (!text) { snprintf(g_status, sizeof(g_status), tr(S_READ_FAIL), g_file); return 0; }

    size_t cap = (size_t)size + (size_t)NP * 64 + 64;
    char* out = (char*)malloc(cap);
    size_t o = 0;
    int defines = 0;
    const char *ls, *le, *p;
    for (p = text; (p = next_line(p, &ls, &le)) != NULL;)
    {
        const char* line_end = p;   // includes the original line break
        const char* s = skip_ws(ls);
        char num[32];
        int done = 0;
        if (strncmp(s, "#define", 7) == 0)
        {
            const char* n0 = skip_ws(s + 7);
            const char* n1 = n0;
            while (n1 < le && *n1 != ' ' && *n1 != '\t') n1++;
            Param* pr = find_param(n0, n1 - n0);
            float v;
            const char* ve;
            if (pr && parse_num(n1, &v, &ve))
            {
                fmt_num(num, sizeof(num), pr->value, decimals(*pr));
                o += snprintf(out + o, cap - o, "#define %-12s %6s", pr->name, num);
                memcpy(out + o, ve, line_end - ve); o += line_end - ve;
                done = 1; defines++;
            }
        }
        else if (strncmp(s, "#pragma parameter", 17) == 0)
        {
            const char* n0 = skip_ws(s + 17);
            const char* n1 = n0;
            while (n1 < le && *n1 != ' ' && *n1 != '\t') n1++;
            Param* pr = find_param(n0, n1 - n0);
            const char* q1 = (const char*)memchr(n1, '"', le - n1);
            const char* q2 = q1 ? (const char*)memchr(q1 + 1, '"', le - q1 - 1) : NULL;
            float v;
            const char* ve;
            if (pr && q2 && parse_num(q2 + 1, &v, &ve))
            {
                fmt_num(num, sizeof(num), pr->value, decimals(*pr));
                memcpy(out + o, ls, q2 + 1 - ls); o += q2 + 1 - ls;
                o += snprintf(out + o, cap - o, " %s", num);
                memcpy(out + o, ve, line_end - ve); o += line_end - ve;
                done = 1;
            }
        }
        if (!done) { memcpy(out + o, ls, line_end - ls); o += line_end - ls; }
    }
    free(text);

    int ok = replace_file(g_file, out, o);
    free(out);
    if (!ok)
    {
        snprintf(g_status, sizeof(g_status), "%s", tr(S_WRITE_FAIL));
        return 0;
    }
    for (int i = 0; i < NP; i++) P[i].saved = P[i].value;
    const char* base = strrchr(g_file, '\\');
    if (!base) base = strrchr(g_file, '/');
    snprintf(g_status, sizeof(g_status), tr(S_SAVED), base ? base + 1 : g_file, defines ? "" : tr(S_DEFAULTS_ONLY));
    return 1;
}

static int preset_matches(int k)
{
    const char* s = PR[k].vals;
    int n = 0;
    while (*s)
    {
        const char* e = s;
        while (*e && *e != ';') e++;
        const char* eq = (const char*)memchr(s, '=', e - s);
        float v;
        if (eq)
        {
            Param* pr = find_param(s, eq - s);
            if (pr && parse_num(eq + 1, &v, NULL))
            {
                if (fabsf(pr->value - v) > pr->step * 0.5f) return 0;
                n++;
            }
        }
        s = *e ? e + 1 : e;
    }
    return n > 0;
}

static int match_preset(void)
{
    for (int k = 0; k < NPR; k++) if (preset_matches(k)) return k;
    return -1;
}

static void apply_preset(int k)
{
    const char* s = PR[k].vals;
    while (*s)
    {
        const char* e = s;
        while (*e && *e != ';') e++;
        const char* eq = (const char*)memchr(s, '=', e - s);
        float v;
        if (eq)
        {
            Param* pr = find_param(s, eq - s);
            if (pr && parse_num(eq + 1, &v, NULL)) pr->value = v;
        }
        s = *e ? e + 1 : e;
    }
    g_preset = k;
}

// lang=xx in install.ini next to the shader: lang= lines are replaced, everything else is kept byte
// for byte; without such a line it is appended, without the file it is created
static int write_lang(int lang)
{
    char path[PATHLEN];
    ini_path(path, sizeof(path), g_file);
    long size = 0;
    char* text = read_file(path, &size);
    size_t cap = (text ? (size_t)size * 2 : 0) + 64;
    char* out = (char*)malloc(cap);
    if (!out) { free(text); return 0; }
    size_t o = 0;
    int done = 0;
    if (text)
    {
        const char *ls, *le, *p, *vs, *ve;
        for (p = text; (p = next_line(p, &ls, &le)) != NULL;)
        {
            if (ini_line(ls, le, "lang", &vs, &ve))
            {
                o += snprintf(out + o, cap - o, "lang=%s", LANG_ID[lang]);
                memcpy(out + o, le, p - le); o += p - le;   // the original line break
                done = 1;
            }
            else { memcpy(out + o, ls, p - ls); o += p - ls; }
        }
        if (!done && o && out[o - 1] != '\n') { memcpy(out + o, "\r\n", 2); o += 2; }
        free(text);
    }
    else o += snprintf(out, cap, "[crt-emu]\r\n");
    if (!done) o += snprintf(out + o, cap - o, "lang=%s\r\n", LANG_ID[lang]);
    int ok = replace_file(path, out, o);
    free(out);
    return ok;
}

// the EN/RU buttons: texts of the new //@lang block and the panel's own strings at once, the live
// and the saved values stay as they are, the choice goes to install.ini. A press on the lit button
// writes it too: the language the panel opened with (the game's, Windows') becomes a choice.
static void set_lang(int lang)
{
    if (lang < 0 || lang >= NLANG || !g_file[0]) return;
    if (lang != g_lang)
    {
        struct Keep { char name[sizeof(P[0].name)]; float value, saved; };
        static Keep keep[MAXP];
        int n = NP;
        for (int i = 0; i < n; i++)
        {
            memcpy(keep[i].name, P[i].name, sizeof(keep[i].name));
            keep[i].value = P[i].value;
            keep[i].saved = P[i].saved;
        }
        char* text = read_file(g_file, NULL);
        if (!text)
        {
            snprintf(g_status, sizeof(g_status), tr(S_READ_FAIL), g_file);
            g_status_ok = 0;
            return;
        }
        g_lang = lang;
        parse_params(text);
        free(text);
        for (int i = 0; i < n; i++)
        {
            Param* pr = find_param(keep[i].name, strlen(keep[i].name));
            if (pr) { pr->value = keep[i].value; pr->saved = keep[i].saved; }
        }
        g_preset = match_preset();
        g_loaded = NP > 0;
        if (!g_loaded)
        {
            // the file lost its parameters meanwhile: close the panel, F12 could not (it needs g_loaded)
#ifdef _WIN32
            InterlockedExchange(&g_crtemu_osd_visible, 0);
#else
            g_crtemu_osd_visible = 0;
#endif
            return;
        }
    }
    g_status[0] = 0;
    if (!write_lang(lang))
    {
        snprintf(g_status, sizeof(g_status), "%s", tr(S_LANG_FAIL));
        g_status_ok = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// input queue (game thread -> render thread)
// ---------------------------------------------------------------------------------------------

struct Ev { unsigned msg; unsigned long long w; long long l; };
enum { QN = 512 };
static Ev g_q[QN];
static int g_qh, g_qt;

#ifdef _WIN32
static CRITICAL_SECTION g_qcs;
static volatile long g_qcs_init;
static void q_lock()
{
    if (InterlockedCompareExchange(&g_qcs_init, 1, 0) == 0) { InitializeCriticalSection(&g_qcs); g_qcs_init = 2; }
    while (g_qcs_init != 2) Sleep(0);
    EnterCriticalSection(&g_qcs);
}
static void q_unlock() { LeaveCriticalSection(&g_qcs); }
#else
static void q_lock() {}
static void q_unlock() {}
#endif

static void q_push(unsigned msg, unsigned long long w, long long l)
{
    q_lock();
    int n = (g_qt + 1) % QN;
    if (n != g_qh) { g_q[g_qt].msg = msg; g_q[g_qt].w = w; g_q[g_qt].l = l; g_qt = n; }
    q_unlock();
}

#ifndef WM_MOUSEMOVE
enum
{
    WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105,
    WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203,
    WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_RBUTTONDBLCLK = 0x206,
    WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208, WM_MBUTTONDBLCLK = 0x209, WM_MOUSEWHEEL = 0x20A
};
#endif

static ImGuiKey vk_to_key(unsigned vk)
{
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    switch (vk)
    {
    case 0x09: return ImGuiKey_Tab;
    case 0x25: return ImGuiKey_LeftArrow;
    case 0x27: return ImGuiKey_RightArrow;
    case 0x26: return ImGuiKey_UpArrow;
    case 0x28: return ImGuiKey_DownArrow;
    case 0x21: return ImGuiKey_PageUp;
    case 0x22: return ImGuiKey_PageDown;
    case 0x24: return ImGuiKey_Home;
    case 0x23: return ImGuiKey_End;
    case 0x2D: return ImGuiKey_Insert;
    case 0x2E: return ImGuiKey_Delete;
    case 0x08: return ImGuiKey_Backspace;
    case 0x20: return ImGuiKey_Space;
    case 0x0D: return ImGuiKey_Enter;
    case 0x1B: return ImGuiKey_Escape;
    case 0xBE: return ImGuiKey_Period;
    case 0xBD: return ImGuiKey_Minus;
    case 0x6E: return ImGuiKey_KeypadDecimal;
    case 0x6D: return ImGuiKey_KeypadSubtract;
    case 0x10: return ImGuiKey_LeftShift;
    case 0x11: return ImGuiKey_LeftCtrl;
    case 0x12: return ImGuiKey_LeftAlt;
    default: return ImGuiKey_None;
    }
}

static void drain_input(ImGuiIO& io)
{
    q_lock();
    while (g_qh != g_qt)
    {
        Ev e = g_q[g_qh];
        g_qh = (g_qh + 1) % QN;
        int x = (short)(e.l & 0xFFFF), y = (short)((e.l >> 16) & 0xFFFF);
        switch (e.msg)
        {
        case WM_MOUSEMOVE: io.AddMousePosEvent((float)x, (float)y); break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: io.AddMousePosEvent((float)x, (float)y); io.AddMouseButtonEvent(0, true); break;
        case WM_LBUTTONUP: io.AddMousePosEvent((float)x, (float)y); io.AddMouseButtonEvent(0, false); break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: io.AddMouseButtonEvent(1, true); break;
        case WM_RBUTTONUP: io.AddMouseButtonEvent(1, false); break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: io.AddMouseButtonEvent(2, true); break;
        case WM_MBUTTONUP: io.AddMouseButtonEvent(2, false); break;
        case WM_MOUSEWHEEL: io.AddMouseWheelEvent(0.0f, (float)(short)((e.w >> 16) & 0xFFFF) / 120.0f); break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        {
            bool down = e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN;
            unsigned vk = (unsigned)e.w;
            if (vk == 0x11) io.AddKeyEvent(ImGuiMod_Ctrl, down);
            if (vk == 0x10) io.AddKeyEvent(ImGuiMod_Shift, down);
            if (vk == 0x12) io.AddKeyEvent(ImGuiMod_Alt, down);
            ImGuiKey k = vk_to_key(vk);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, down);
            break;
        }
        case WM_CHAR:
            if (e.w >= 32 && e.w != 127) io.AddInputCharacter((unsigned)e.w);
            break;
        }
    }
    q_unlock();
}

#ifdef _WIN32
static HHOOK g_msg_hook;

static int is_input_msg(UINT m)
{
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST);
}

static LRESULT CALLBACK msg_hook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0 && g_crtemu_osd_visible && lParam)
    {
        MSG* m = (MSG*)lParam;
        if (is_input_msg(m->message))
        {
            if (wParam == PM_REMOVE) q_push(m->message, (unsigned long long)m->wParam, (long long)m->lParam);
            m->message = WM_NULL;
        }
    }
    return CallNextHookEx(g_msg_hook, code, wParam, lParam);
}
#endif

extern "C" void crtemu_osd_toggle(void* hwnd)
{
    if (!g_loaded) return;
    long show = !g_crtemu_osd_visible;
#ifdef _WIN32
    if (show)
    {
        HWND w = (HWND)hwnd;
        DWORD tid = w ? GetWindowThreadProcessId(w, NULL) : GetCurrentThreadId();
        if (!g_msg_hook) g_msg_hook = SetWindowsHookExA(WH_GETMESSAGE, msg_hook, NULL, tid);
        if (!g_msg_hook) return;   // without the hook clicks would go through to the game
        POINT pt;
        if (w && GetCursorPos(&pt) && ScreenToClient(w, &pt))
            q_push(WM_MOUSEMOVE, 0, (long long)(((pt.y & 0xFFFF) << 16) | (pt.x & 0xFFFF)));
    }
#else
    (void)hwnd;
#endif
#ifdef _WIN32
    InterlockedExchange(&g_crtemu_osd_visible, show);
#else
    g_crtemu_osd_visible = show;
#endif
}

// ---------------------------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------------------------

static ImGuiContext* g_ctx;
static int g_gl_ready;
static float g_scale = 1.0f;
static double g_last_time;

static double now_seconds()
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

static float dpi_scale(void* hwnd)
{
#ifdef _WIN32
    typedef UINT (WINAPI *GetDpiForWindowProc)(HWND);
    static GetDpiForWindowProc get_dpi = (GetDpiForWindowProc)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    UINT dpi = 0;
    if (get_dpi && hwnd) dpi = get_dpi((HWND)hwnd);
    if (!dpi)
    {
        HDC dc = GetDC(NULL);
        dpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(NULL, dc);
    }
    return dpi >= 96 ? dpi / 96.0f : 1.0f;
#else
    (void)hwnd;
    const char* s = getenv("CRTEMU_OSD_SCALE");
    return s ? (float)atof(s) : 1.0f;
#endif
}

static void load_font(ImGuiIO& io)
{
    const char* names[] = { "segoeui.ttf", "tahoma.ttf", "arial.ttf" };
    char path[PATHLEN];
#ifdef _WIN32
    char windir[MAX_PATH];
    UINT n = GetWindowsDirectoryA(windir, sizeof(windir));
    if (!n || n >= sizeof(windir)) strcpy(windir, "C:\\Windows");
    for (int i = 0; i < 3; i++)
    {
        snprintf(path, sizeof(path), "%s\\Fonts\\%s", windir, names[i]);
        if (file_exists(path) && io.Fonts->AddFontFromFileTTF(path, 17.0f)) return;
    }
#else
    (void)names;
    const char* f = getenv("CRTEMU_OSD_FONT");
    snprintf(path, sizeof(path), "%s", f ? f : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    if (file_exists(path) && io.Fonts->AddFontFromFileTTF(path, 17.0f)) return;
#endif
    io.Fonts->AddFontDefault();
}

static ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

static void setup_style(float s)
{
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 10.0f;
    st.FrameRounding = 5.0f;
    st.GrabRounding = 4.0f;
    st.PopupRounding = 6.0f;
    st.ScrollbarRounding = 6.0f;
    st.WindowPadding = ImVec2(14, 12);
    st.FramePadding = ImVec2(8, 4);
    st.ItemSpacing = ImVec2(8, 6);
    st.WindowBorderSize = 1.0f;
    st.GrabMinSize = 12.0f;
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    ImVec4* c = st.Colors;
    const ImVec4 accent = rgb(226, 166, 86), accent_hi = rgb(240, 186, 110), ink = rgb(235, 230, 220);
    c[ImGuiCol_Text] = ink;
    c[ImGuiCol_TextDisabled] = rgb(155, 149, 139);
    c[ImGuiCol_WindowBg] = rgb(24, 23, 26, 0.93f);
    c[ImGuiCol_PopupBg] = rgb(30, 29, 33, 0.98f);
    c[ImGuiCol_Border] = rgb(60, 57, 64);
    c[ImGuiCol_TitleBg] = rgb(24, 23, 26, 0.95f);
    c[ImGuiCol_TitleBgActive] = rgb(34, 32, 37, 0.98f);
    c[ImGuiCol_FrameBg] = rgb(40, 38, 44);
    c[ImGuiCol_FrameBgHovered] = rgb(52, 49, 57);
    c[ImGuiCol_FrameBgActive] = rgb(60, 56, 66);
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_Button] = rgb(44, 42, 48);
    c[ImGuiCol_ButtonHovered] = rgb(60, 56, 66);
    c[ImGuiCol_ButtonActive] = rgb(226, 166, 86, 0.55f);
    c[ImGuiCol_Header] = rgb(44, 42, 48);
    c[ImGuiCol_HeaderHovered] = rgb(58, 54, 63);
    c[ImGuiCol_HeaderActive] = rgb(66, 61, 71);
    c[ImGuiCol_Separator] = rgb(60, 57, 64);
    c[ImGuiCol_ScrollbarGrab] = rgb(70, 66, 76);
    c[ImGuiCol_ResizeGrip] = rgb(226, 166, 86, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = rgb(226, 166, 86, 0.6f);
    c[ImGuiCol_ResizeGripActive] = accent;
    st.ScaleAllSizes(s);
    st.FontScaleDpi = s;
}

static void ensure_context(void* hwnd)
{
    if (g_ctx) return;
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.MouseDrawCursor = true;                  // the game hides the system cursor
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    g_scale = dpi_scale(hwnd);
    if (g_scale < 1.0f) g_scale = 1.0f;
    if (g_scale > 4.0f) g_scale = 4.0f;
    setup_style(g_scale);
    load_font(io);
    g_last_time = now_seconds();
}

static int dirty(void)
{
    for (int i = 0; i < NP; i++) if (P[i].value != P[i].saved) return 1;
    return 0;
}

static const char* group_label(const char* id)
{
    for (int i = 0; i < NG; i++) if (strcmp(G[i].id, id) == 0) return G[i].label;
    return id[0] ? id : tr(S_PARAMS);
}

static float scalar_f(ImGuiDataType type, const void* p)
{
    return type == ImGuiDataType_Float ? *(const float*)p : (float)*(const int*)p;
}

// A slider drawn as a thin line with a pill-shaped handle that carries the value, so the number
// is never under the handle. The handle is as wide as the widest value of this slider and does not
// change size. Input is ImGui's own, as in ImGui::SliderScalar: drag, Ctrl+click or Enter to type
// a number, arrows. The pill is the grab rectangle SliderBehavior returns.
static bool pill_slider(const char* id_label, ImGuiDataType type, void* v, const void* vmin, const void* vmax,
                        const char* fmt, ImGuiSliderFlags flags)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& st = g.Style;
    const ImGuiID id = window->GetID(id_label);
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect bb(pos, ImVec2(pos.x + ImGui::CalcItemWidth(), pos.y + ImGui::GetFrameHeight()));
    ImGui::ItemSize(bb, st.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id, &bb, ImGuiItemFlags_Inputable)) return false;

    const bool hovered = ImGui::ItemHoverable(bb, id, g.LastItemData.ItemFlags);
    bool typing = ImGui::TempInputIsActive(id);
    if (!typing)
    {
        const bool clicked = hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, id);
        const bool activate = clicked || g.NavActivateId == id;
        if (activate && clicked) ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
        if (activate && ((clicked && g.IO.KeyCtrl) || (g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput))))
            typing = true;
        if (activate) memcpy(&g.ActiveIdValueOnActivation, v, ImGui::DataTypeGetInfo(type)->Size);
        if (activate && !typing)
        {
            ImGui::SetActiveID(id, window);
            ImGui::SetFocusID(id, window);
            ImGui::FocusWindow(window);
            g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
        }
    }
    if (typing)
    {
        const bool clamp = (flags & ImGuiSliderFlags_ClampOnInput) != 0;
        return ImGui::TempInputScalar(bb, id, id_label, type, v, fmt, clamp ? vmin : NULL, clamp ? vmax : NULL);
    }

    // pill width: the widest of the two ends and the current value, plus padding
    char text[64], end_text[64];
    float text_w = 0.0f;
    const void* ends[2] = { vmin, vmax };
    for (int i = 0; i < 2; i++)
    {
        ImGui::DataTypeFormatString(end_text, sizeof(end_text), type, ends[i], fmt);
        text_w = ImMax(text_w, ImGui::CalcTextSize(end_text).x);
    }
    ImGui::DataTypeFormatString(text, sizeof(text), type, v, fmt);
    const ImVec2 text_sz = ImGui::CalcTextSize(text);
    text_w = ImMax(text_w, text_sz.x);

    ImRect grab;
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, text_w + st.FramePadding.x * 2.0f);
    const bool changed = ImGui::SliderBehavior(bb, id, type, v, vmin, vmax, fmt, flags, &grab);
    ImGui::PopStyleVar();
    if (changed)
    {
        ImGui::MarkItemEdited(id);
        ImGui::DataTypeFormatString(text, sizeof(text), type, v, fmt);
    }

    // the line: dim, amber from the start (or from zero, if zero is inside the range) to the pill
    ImDrawList* dl = window->DrawList;
    const bool active = g.ActiveId == id;
    const float s = g_scale, half = ImMax(1.0f, floorf(1.0f * s));
    const float cy = floorf((bb.Min.y + bb.Max.y) * 0.5f);
    const float x0 = bb.Min.x + 2.0f, x1 = bb.Max.x - 2.0f;           // 2: ImGui's grab padding
    const float gw = grab.Max.x - grab.Min.x, pill_x = (grab.Min.x + grab.Max.x) * 0.5f;
    const float lo = scalar_f(type, vmin), hi = scalar_f(type, vmax);
    float from = x0;
    if (lo < 0.0f && hi > 0.0f)
        from = x0 + gw * 0.5f + (x1 - x0 - gw) * (0.0f - lo) / (hi - lo);
    ImGui::RenderNavCursor(bb, id);
    dl->AddRectFilled(ImVec2(x0, cy - half), ImVec2(x1, cy + half),
                      ImGui::GetColorU32(hovered || active ? rgb(84, 79, 90) : rgb(66, 62, 72)), half);
    dl->AddRectFilled(ImVec2(ImMin(from, pill_x), cy - half), ImVec2(ImMax(from, pill_x), cy + half),
                      ImGui::GetColorU32(rgb(226, 166, 86, 0.85f)), half);

    // the pill with the value
    const float ph = floorf(ImGui::GetFrameHeight() - 6.0f * s);
    const ImVec2 p0(floorf(grab.Min.x), floorf(cy - ph * 0.5f)), p1(floorf(grab.Max.x), p0.y + ph);
    const ImVec4 pill = active ? rgb(252, 208, 140) : hovered ? rgb(240, 186, 110) : rgb(226, 166, 86);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(pill), ph * 0.5f);
    dl->AddText(ImVec2(floorf((p0.x + p1.x - text_sz.x) * 0.5f), floorf(cy - text_sz.y * 0.5f)),
                ImGui::GetColorU32(rgb(28, 24, 20)), text);
    return changed;
}

static void param_row(Param& p, float label_w)
{
    ImGui::PushID(p.name);
    bool changed = p.value != p.saved;
    ImGui::AlignTextToFramePadding();
    if (changed) ImGui::PushStyleColor(ImGuiCol_Text, rgb(240, 186, 110));
    ImGui::TextUnformatted(p.label[0] ? p.label : p.name);
    if (changed) ImGui::PopStyleColor();
    if (p.hint[0] && ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(p.hint);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (p.nopt > 0)
    {
        int cur = (int)floorf(p.value + 0.5f);
        if (cur < 0) cur = 0;
        if (cur >= p.nopt) cur = p.nopt - 1;
        if (ImGui::BeginCombo("##v", p.opt[cur]))
        {
            for (int i = 0; i < p.nopt; i++)
                if (ImGui::Selectable(p.opt[i], i == cur)) { p.value = (float)i; g_preset = -1; }
            ImGui::EndCombo();
        }
    }
    else
    {
        float v = p.value;
        if (pill_slider("##v", ImGuiDataType_Float, &v, &p.min, &p.max, decimals(p) == 3 ? "%.3f" : "%.2f", ImGuiSliderFlags_AlwaysClamp))
        {
            if (p.step > 0.0f) v = p.min + floorf((v - p.min) / p.step + 0.5f) * p.step;
            if (v < p.min) v = p.min;
            if (v > p.max) v = p.max;
            p.value = v;
            g_preset = -1;
        }
    }
    if (p.hint[0] && ImGui::BeginItemTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(p.hint);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) p.value = p.saved;
    ImGui::PopID();
}

// ---------------------------------------------------------------------------------------------
// magnifier: a round lens under the panel with the screen's own pixels enlarged, zoom x zoom
// each, so the mask and the lines can be seen (and shot) at their real size. Every frame the
// square under the lens is copied from the back buffer, which still holds only the shaded frame:
// ImGui draws later, in RenderDrawData. Not saved, it lives while the game runs.
// ---------------------------------------------------------------------------------------------

typedef void (APIENTRY *CopyTexSubImage2DProc)(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                               GLint x, GLint y, GLsizei width, GLsizei height);

enum { LENS_ZOOM_MIN = 2, LENS_ZOOM_MAX = 8 };

static bool   g_lens_on;
static int    g_lens_zoom = 4;
static int    g_lens_d;                         // diameter in screen pixels, 0 = default
static float  g_lens_cx = -1.0f, g_lens_cy;     // magnified point, < 0 = middle of the screen
static float  g_lens_wheel;                     // wheel movement not yet turned into zoom steps
static GLuint g_lens_tex;
static int    g_lens_tex_n;                     // side of g_lens_tex in pixels
static CopyTexSubImage2DProc g_copy_tex_sub;    // GL 1.1, not in the backend's loader

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int lens_d_min(void) { return (int)(160 * g_scale); }

static int lens_d_max(void)
{
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    int m = (int)((ds.x < ds.y ? ds.x : ds.y) * 0.9f);
    return m > lens_d_min() ? m : lens_d_min();
}

static void lens_clamp(void)
{
    if (g_lens_d <= 0) g_lens_d = (int)(360 * g_scale);
    if (g_lens_d < lens_d_min()) g_lens_d = lens_d_min();
    if (g_lens_d > lens_d_max()) g_lens_d = lens_d_max();
    if (g_lens_zoom < LENS_ZOOM_MIN) g_lens_zoom = LENS_ZOOM_MIN;
    if (g_lens_zoom > LENS_ZOOM_MAX) g_lens_zoom = LENS_ZOOM_MAX;
}

// copy the n x n square at (x, y) (top-left, screen pixels) of the back buffer into g_lens_tex
static void lens_grab(int x, int y, int n, int fb_height)
{
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    if (!g_lens_tex) glGenTextures(1, &g_lens_tex);
    glBindTexture(GL_TEXTURE_2D, g_lens_tex);
    if (g_lens_tex_n != n)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g_lens_tex_n = n;
    }
    g_copy_tex_sub(GL_TEXTURE_2D, 0, 0, 0, x, fb_height - y - n, n, n);   // GL rows go bottom-up
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
}

// before the panel: the window is created at the bottom of ImGui's window list
// (NoBringToFrontOnFocus) and stays there, under the panel
static void lens_window(void)
{
    if (!g_lens_on || !g_copy_tex_sub) return;
    lens_clamp();
    ImGuiIO& io = ImGui::GetIO();
    // H: the window's last row is left out. With nonexclusive=true (as the setup writes ddraw.ini)
    // cnc-ddraw makes the window one row taller than the screen and never draws that row
    // (render.height + 1 and opengl_y_align in dd.c). Elsewhere this only loses the bottom row.
    const int W = (int)io.DisplaySize.x, H = (int)io.DisplaySize.y - 1;
    const int z = g_lens_zoom;
    const int n = (g_lens_d + z / 2) / z;              // screen pixels across the lens
    const float L = (float)(n * z), r = L * 0.5f;      // lens side on screen
    const float h = n * 0.5f;
    // the magnified point goes right up to the screen edges, the lens stays whole on the screen:
    // near an edge it stops and the enlarged area slides on under it
    if (g_lens_cx < 0.0f) { g_lens_cx = W * 0.5f; g_lens_cy = H * 0.5f; }
    g_lens_cx = clampf(g_lens_cx, h, W - h);
    g_lens_cy = clampf(g_lens_cy, h, H - h);
    const float lx = clampf(g_lens_cx, r, W - r), ly = clampf(g_lens_cy, r, H - r);
    // whole-pixel corner: every screen pixel becomes an even z x z block
    const ImVec2 p0(floorf(lx - r), floorf(ly - r)), p1(p0.x + L, p0.y + L);

    ImGui::SetNextWindowPos(p0, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(L, L), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool shown = ImGui::Begin("##crtemu_lens", NULL,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (shown)
    {
        // drag to move, wheel to zoom; both take effect in the next frame
        ImGui::InvisibleButton("##drag", ImVec2(L, L));
        if (ImGui::IsItemActive()) { g_lens_cx += io.MouseDelta.x; g_lens_cy += io.MouseDelta.y; }
        // one zoom step per notch: a high-resolution wheel or a touchpad sends fractions of one
        g_lens_wheel = ImGui::IsItemHovered() ? g_lens_wheel + io.MouseWheel : 0.0f;
        const int steps = (int)g_lens_wheel;
        if (steps)
        {
            g_lens_wheel -= (float)steps;
            g_lens_zoom += steps;
            lens_clamp();
        }

        // the n x n square around the magnified point (inside the screen, see the clamps above)
        int sx = (int)floorf(g_lens_cx - h), sy = (int)floorf(g_lens_cy - h);
        if (sx > W - n) sx = W - n;
        if (sy > H - n) sy = H - n;
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        lens_grab(sx, sy, n, (int)io.DisplaySize.y);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        const ImVec2 c(p0.x + r, p0.y + r);
        const float s = g_scale;
        if (pio.DrawCallback_SetSamplerNearest) dl->AddCallback(pio.DrawCallback_SetSamplerNearest, NULL);
        dl->AddImageRounded((ImTextureID)g_lens_tex, p0, p1, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f), IM_COL32_WHITE, r);
        if (pio.DrawCallback_SetSamplerLinear) dl->AddCallback(pio.DrawCallback_SetSamplerLinear, NULL);
        dl->AddCircle(c, r + 1.5f * s, IM_COL32(12, 11, 13, 220), 0, 3.0f * s);
        dl->AddCircle(c, r, ImGui::GetColorU32(ImGuiCol_SliderGrab), 0, 1.5f * s);
    }
    ImGui::End();
}

static int g_lang_pending = -1;         // EN/RU pressed: switched before the next frame's widgets

static void draw_ui(void)
{
    if (g_lang_pending >= 0) { set_lang(g_lang_pending); g_lang_pending = -1; }
    lens_window();
    ImGuiIO& io = ImGui::GetIO();
    const float s = g_scale;
    ImGui::SetNextWindowPos(ImVec2(28 * s, 28 * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(360 * s, 0), ImVec2(io.DisplaySize.x - 56 * s, io.DisplaySize.y - 56 * s));
    ImGui::SetNextWindowSize(ImVec2(470 * s, 0), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin(tr(S_TITLE), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    }

    // presets and A/B
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(tr(S_AB)).x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
    if (NPR > 0 && ImGui::BeginCombo("##preset", g_preset >= 0 ? PR[g_preset].name : tr(dirty() ? S_OWN_DIRTY : S_OWN)))
    {
        for (int i = 0; i < NPR; i++)
            if (ImGui::Selectable(PR[i].name, i == g_preset)) apply_preset(i);
        ImGui::EndCombo();
    }
    else if (NPR == 0) ImGui::TextDisabled("%s", tr(S_NO_PRESETS));
    ImGui::SameLine();
    bool off = g_crtemu_shader_off != 0;
    if (off) ImGui::PushStyleColor(ImGuiCol_Button, rgb(226, 166, 86, 0.55f));
    if (ImGui::Button(tr(S_AB)))
    {
#ifdef _WIN32
        InterlockedExchange(&g_crtemu_shader_off, !g_crtemu_shader_off);
#else
        g_crtemu_shader_off = !g_crtemu_shader_off;
#endif
    }
    if (off) ImGui::PopStyleColor();

    // label column width: widest label + gap (parameters and the magnifier rows)
    float label_w = ImMax(ImGui::CalcTextSize(tr(S_LENS_ZOOM)).x, ImGui::CalcTextSize(tr(S_LENS_SIZE)).x);
    for (int i = 0; i < NP; i++)
    {
        float w = ImGui::CalcTextSize(P[i].label[0] ? P[i].label : P[i].name).x;
        if (w > label_w) label_w = w;
    }
    label_w += ImGui::GetStyle().WindowPadding.x + 14.0f * s;

    // magnifier: on / off, then zoom and size as rows like the parameters
    ImGui::BeginDisabled(!g_copy_tex_sub);
    ImGui::Checkbox(tr(S_LENS), &g_lens_on);
    ImGui::SetItemTooltip("%s", tr(S_LENS_TIP));
    if (g_lens_on)
    {
        lens_clamp();
        const int zmin = LENS_ZOOM_MIN, zmax = LENS_ZOOM_MAX, dmin = lens_d_min(), dmax = lens_d_max();
        const struct { int label; const char* id; int* v; const int* lo; const int* hi; const char* fmt; } rows[2] = {
            { S_LENS_ZOOM, "##lens_zoom", &g_lens_zoom, &zmin, &zmax, "×%d" },
            { S_LENS_SIZE, "##lens_size", &g_lens_d, &dmin, &dmax, tr(S_LENS_PX) },
        };
        for (int i = 0; i < 2; i++)
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr(rows[i].label));
            ImGui::SameLine(label_w);
            ImGui::SetNextItemWidth(-FLT_MIN);
            pill_slider(rows[i].id, ImGuiDataType_S32, rows[i].v, rows[i].lo, rows[i].hi, rows[i].fmt, ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("%s", tr(S_LENS_TIP));
        }
    }
    ImGui::EndDisabled();

    int d = dirty();
    ImGui::BeginDisabled(!d);
    if (ImGui::Button(tr(S_SAVE))) g_status_ok = save_params();
    ImGui::SameLine();
    if (ImGui::Button(tr(S_REVERT)))
    {
        for (int i = 0; i < NP; i++) P[i].value = P[i].saved;
        g_preset = match_preset();
        g_status[0] = 0;
    }
    ImGui::EndDisabled();
    if (d && !(g_status[0] && !g_status_ok))
    {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(rgb(240, 186, 110), "%s", tr(S_UNSAVED));
    }
    // panel language: EN / RU at the right end of the Save row, the current one lit
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        float w = st.ItemSpacing.x * (NLANG - 1);
        for (int i = 0; i < NLANG; i++) w += ImGui::CalcTextSize(LANG_BTN[i]).x + st.FramePadding.x * 2;
        ImGui::SameLine();
        float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w;
        if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
        for (int i = 0; i < NLANG; i++)
        {
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            if (i == g_lang) ImGui::PushStyleColor(ImGuiCol_Button, rgb(226, 166, 86, 0.55f));
            if (ImGui::Button(LANG_BTN[i])) g_lang_pending = i;
            if (i == g_lang) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("%s", tr(S_LANG_TIP));
            ImGui::PopID();
        }
    }
    // a long status must not widen the auto-sized window: wrap it at the current width
    if (g_status[0] && (!g_status_ok || !d))
    {
        ImGui::PushTextWrapPos(0.0f);
        if (!g_status_ok) ImGui::TextColored(rgb(235, 110, 90), "%s", g_status);
        else ImGui::TextDisabled("%s", g_status);
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();

    ImGui::PushItemWidth(-FLT_MIN);

    // groups in declared order, then parameters without a known group
    int shown[MAXP] = { 0 };
    for (int gi = 0; gi <= NG; gi++)
    {
        const char* gid = gi < NG ? G[gi].id : NULL;
        int count = 0;
        for (int i = 0; i < NP; i++)
            if (!shown[i] && (gid ? strcmp(P[i].group, gid) == 0 : 1)) count++;
        if (!count) continue;
        int open_default = !gid || strcmp(gid, "light") == 0 || strcmp(gid, "beam") == 0 || strcmp(gid, "screen") == 0;
        // the ID is the group id, not its label: a language switch keeps groups open or closed
        char head[sizeof(G[0].label) + sizeof(G[0].id) + 8];
        snprintf(head, sizeof(head), "%s###grp_%s", gid ? G[gi].label : group_label(""), gid ? gid : "");
        if (ImGui::CollapsingHeader(head, open_default ? ImGuiTreeNodeFlags_DefaultOpen : 0))
        {
            for (int i = 0; i < NP; i++)
                if (!shown[i] && (gid ? strcmp(P[i].group, gid) == 0 : 1)) { param_row(P[i], label_w); shown[i] = 1; }
        }
        else
        {
            for (int i = 0; i < NP; i++)
                if (gid ? strcmp(P[i].group, gid) == 0 : 1) shown[i] = 1;
        }
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr(S_FOOTER));
    ImGui::PopTextWrapPos();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive()) open = false;
    ImGui::End();

    if (!open)
    {
#ifdef _WIN32
        InterlockedExchange(&g_crtemu_osd_visible, 0);
#else
        g_crtemu_osd_visible = 0;
#endif
    }
}

extern "C" void crtemu_osd_render(void* hwnd, int width, int height)
{
    if (!g_crtemu_osd_visible || !g_loaded || width <= 0 || height <= 0) return;
    ensure_context(hwnd);
    ImGui::SetCurrentContext(g_ctx);
    if (!g_gl_ready)
    {
        if (!ImGui_ImplOpenGL3_Init(NULL)) return;
        g_gl_ready = 1;
        g_copy_tex_sub = (CopyTexSubImage2DProc)imgl3wGetProcAddress("glCopyTexSubImage2D");
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)width, (float)height);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    double t = now_seconds();
    io.DeltaTime = (float)(t - g_last_time);
    if (io.DeltaTime <= 0.0f || io.DeltaTime > 0.5f) io.DeltaTime = 1.0f / 60.0f;
    g_last_time = t;
    drain_input(io);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

extern "C" void crtemu_osd_gl_shutdown(void)
{
    if (g_ctx && g_gl_ready)
    {
        ImGui::SetCurrentContext(g_ctx);
        if (g_lens_tex) glDeleteTextures(1, &g_lens_tex);   // before the backend unloads its GL functions
        g_lens_tex = 0;
        g_lens_tex_n = 0;
        g_copy_tex_sub = NULL;
        ImGui_ImplOpenGL3_Shutdown();
        g_gl_ready = 0;
    }
}

#ifdef CRTEMU_OSD_TEST
// host test driver: feed synthetic input and draw one frame into the current GL framebuffer
extern "C" void crtemu_osd_test_input(unsigned msg, unsigned long long w, long long l) { q_push(msg, w, l); }
extern "C" void crtemu_osd_test_show(int on) { g_crtemu_osd_visible = on; }
extern "C" const char* crtemu_osd_test_status(void) { return g_status; }
extern "C" void crtemu_osd_test_set(const char* name, float v) { Param* p = find_param(name, strlen(name)); if (p) p->value = v; }
extern "C" int crtemu_osd_test_save(void) { return save_params(); }
// what the panel would show: language, parameters, groups, options, presets, its own strings
extern "C" const char* crtemu_osd_test_lang(void) { return LANG_ID[g_lang]; }
extern "C" int crtemu_osd_test_count(int what) { return what == 0 ? NP : what == 1 ? NG : NPR; }
extern "C" const char* crtemu_osd_test_label(int i) { return i >= 0 && i < NP ? P[i].label : ""; }
extern "C" const char* crtemu_osd_test_hint(int i) { return i >= 0 && i < NP ? P[i].hint : ""; }
extern "C" const char* crtemu_osd_test_option(int i, int k) { return i >= 0 && i < NP && k >= 0 && k < P[i].nopt ? P[i].opt[k] : ""; }
extern "C" const char* crtemu_osd_test_group(int i) { return i >= 0 && i < NG ? G[i].label : ""; }
extern "C" const char* crtemu_osd_test_preset(int i) { return i >= 0 && i < NPR ? PR[i].name : ""; }
extern "C" const char* crtemu_osd_test_ui(int s) { return s >= 0 && s < S_COUNT ? tr(s) : ""; }
// what the EN/RU buttons do (without waiting for a frame)
extern "C" void crtemu_osd_test_set_lang(int lang) { set_lang(lang); }
#endif
