/*
 * config.c - parser INI estrito e reload atomico do ntwm.
 *
 * O parser comum e propositalmente tolerante para units; configuracao de
 * desktop nao pode ser. Aqui qualquer secao, chave, conversao ou linha invalida
 * rejeita o arquivo inteiro. O estado vivo so e trocado depois do EOF valido.
 */
#include "ntwm.h"
#include <ctype.h>
#include <stdarg.h>

static int cfg_error(char *out, size_t cap, int line, const char *fmt, ...)
{
    char detail[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    snprintf(out, cap, "ntwm.conf:%d: %s", line, detail);
    return 0;
}

static int parse_long_strict(const char *s, long minv, long maxv, int *out)
{
    char *end;
    long v = strtol(s, &end, 0);
    while (*end == ' ' || *end == '\t') end++;
    if (end == s || *end || v < minv || v > maxv)
        return 0;
    *out = (int)v;
    return 1;
}

static int parse_float_strict(const char *s, float minv, float maxv, float *out)
{
    char *end;
    double v = strtod(s, &end);
    while (*end == ' ' || *end == '\t') end++;
    if (end == s || *end || v < minv || v > maxv)
        return 0;
    *out = (float)v;
    return 1;
}

static int parse_bool(const char *s, int *out)
{
    if (!_stricmp(s, "true") || !_stricmp(s, "yes") ||
        !_stricmp(s, "on") || !strcmp(s, "1")) {
        *out = 1;
        return 1;
    }
    if (!_stricmp(s, "false") || !_stricmp(s, "no") ||
        !_stricmp(s, "off") || !strcmp(s, "0")) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int parse_layout(const char *s, LayoutKind *out)
{
    if (!_stricmp(s, "dwindle")) { *out = LAYOUT_DWINDLE; return 1; }
    if (!_stricmp(s, "master"))  { *out = LAYOUT_MASTER; return 1; }
    return 0;
}

static int parse_orientation(const char *s, MasterOrientation *out)
{
    if (!_stricmp(s, "left"))   { *out = MASTER_LEFT; return 1; }
    if (!_stricmp(s, "right"))  { *out = MASTER_RIGHT; return 1; }
    if (!_stricmp(s, "top"))    { *out = MASTER_TOP; return 1; }
    if (!_stricmp(s, "bottom")) { *out = MASTER_BOTTOM; return 1; }
    if (!_stricmp(s, "center")) { *out = MASTER_CENTER; return 1; }
    return 0;
}

static int parse_color(const char *s, unsigned *out)
{
    if (*s == '#') s++;
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || *end || v > 0xfffffful)
        return 0;
    *out = (unsigned)v;
    return 1;
}

static int set_base_mod(WmConfig *cfg, const char *value)
{
    unsigned next;
    if (!wm_parse_mods(value, &next) || next == 0)
        return 0;
    unsigned old = cfg->mod;
    cfg->mod = next;
    for (int i = 0; i < cfg->nbinds; i++)
        if (cfg->binds[i].mods & old)
            cfg->binds[i].mods = (cfg->binds[i].mods & ~old) | next;
    return 1;
}

int wm_parse_mods(const char *s, unsigned *mods)
{
    char buf[128];
    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    *mods = 0;
    for (char *p = buf; *p; p++)
        if (*p == '+') *p = ' ';
    char *tok = strtok(buf, " \t");
    if (!tok)
        return 1;
    do {
        if (!_stricmp(tok, "super") || !_stricmp(tok, "win"))
            *mods |= MOD_WIN;
        else if (!_stricmp(tok, "alt"))
            *mods |= MOD_ALT;
        else if (!_stricmp(tok, "shift"))
            *mods |= MOD_SHIFT;
        else if (!_stricmp(tok, "ctrl") || !_stricmp(tok, "control"))
            *mods |= MOD_CTRL;
        else if (_stricmp(tok, "none"))
            return 0;
    } while ((tok = strtok(NULL, " \t")) != NULL);
    return 1;
}

int wm_parse_key(const char *s, unsigned *vk)
{
    if (s[0] && !s[1]) {
        *vk = (unsigned)toupper((unsigned char)s[0]);
        return 1;
    }
    struct { const char *name; unsigned vk; } names[] = {
        { "Return", VK_RETURN }, { "Enter", VK_RETURN },
        { "Space", VK_SPACE }, { "Tab", VK_TAB }, { "Escape", VK_ESCAPE },
        { "Left", VK_LEFT }, { "Right", VK_RIGHT },
        { "Up", VK_UP }, { "Down", VK_DOWN },
        { "Backspace", VK_BACK }, { "Delete", VK_DELETE },
        { "Home", VK_HOME }, { "End", VK_END },
        { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT },
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!_stricmp(s, names[i].name)) {
            *vk = names[i].vk;
            return 1;
        }
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        int f = atoi(s + 1);
        if (f >= 1 && f <= 12) {
            *vk = VK_F1 + (unsigned)(f - 1);
            return 1;
        }
    }
    return 0;
}

static void bind_add(WmConfig *cfg, unsigned mods, unsigned vk,
                     const char *action, const char *arg)
{
    if (cfg->nbinds >= WM_MAX_BINDS)
        return;
    WmBind *b = &cfg->binds[cfg->nbinds++];
    ZeroMemory(b, sizeof *b);
    b->mods = mods;
    b->vk = vk;
    strncpy(b->action, action, sizeof b->action - 1);
    if (arg)
        strncpy(b->arg, arg, sizeof b->arg - 1);
}

static void default_bind(WmConfig *cfg, unsigned extra, unsigned vk,
                         const char *action, const char *arg)
{
    bind_add(cfg, cfg->mod | extra, vk, action, arg);
}

void wm_config_defaults(WmConfig *cfg)
{
    ZeroMemory(cfg, sizeof *cfg);
    /* Alt nao colide com os atalhos Mod/Super do Niri usado no host de
     * desenvolvimento e continua pratico dentro do virt-viewer. */
    cfg->mod = MOD_ALT;
    cfg->default_layout = LAYOUT_DWINDLE;
    cfg->gap_inner = 8;
    cfg->gap_outer = 12;
    cfg->style.border = 2;
    cfg->style.border_rgb = 0x7aa2f7;
    cfg->style.opacity = 238;
    cfg->style.shadow = 1;
    cfg->style.radius = 10;
    cfg->style.animate = 1;
    cfg->style.titlebar = 1;
    cfg->animations = 1;
    cfg->move_ms = 180;
    cfg->open_ms = 160;
    cfg->workspace_ms = 220;
    cfg->focus_ms = 120;
    cfg->dwindle_default_ratio = 50;
    cfg->drag_threshold = 6;
    cfg->nmaster = 1;
    cfg->mfact = 0.55f;
    cfg->master_orientation = MASTER_LEFT;
    cfg->insert_master = 1;
    cfg->bar_enabled = 1;
    cfg->bar_height = 34;

    default_bind(cfg, 0, VK_RETURN, "spawn", "");
    default_bind(cfg, 0, 'Q', "close", "");
    default_bind(cfg, 0, 'H', "focus", "left");
    default_bind(cfg, 0, 'L', "focus", "right");
    default_bind(cfg, 0, 'K', "focus", "up");
    default_bind(cfg, 0, 'J', "focus", "down");
    default_bind(cfg, MOD_SHIFT, 'H', "move", "left");
    default_bind(cfg, MOD_SHIFT, 'L', "move", "right");
    default_bind(cfg, MOD_SHIFT, 'K', "move", "up");
    default_bind(cfg, MOD_SHIFT, 'J', "move", "down");
    default_bind(cfg, MOD_CTRL, 'H', "resize", "left -0.05");
    default_bind(cfg, MOD_CTRL, 'L', "resize", "right 0.05");
    default_bind(cfg, MOD_CTRL, 'K', "resize", "up -0.05");
    default_bind(cfg, MOD_CTRL, 'J', "resize", "down 0.05");
    default_bind(cfg, 0, VK_SPACE, "togglefloating", "");
    default_bind(cfg, 0, 'F', "fullscreen", "");
    default_bind(cfg, 0, 'M', "maximize", "");
    default_bind(cfg, 0, 'T', "layout", "toggle");
    default_bind(cfg, MOD_SHIFT, 'R', "reload", "");
    default_bind(cfg, MOD_SHIFT, 'C', "spawn", "cmd.exe");
    default_bind(cfg, MOD_SHIFT, 'P', "spawn", "powershell.exe");
    for (int i = 0; i < NTUWM_WS; i++) {
        char arg[8];
        snprintf(arg, sizeof arg, "%d", i + 1);
        default_bind(cfg, 0, (unsigned)('1' + i), "workspace", arg);
        default_bind(cfg, MOD_SHIFT, (unsigned)('1' + i),
                     "movetoworkspace", arg);
    }
}

static int action_known(const char *s)
{
    static const char *known[] = {
        "spawn", "close", "focus", "move", "workspace", "movetoworkspace",
        "togglefloating", "fullscreen", "maximize", "layout", "layoutmsg",
        "resize", "reload", "quit"
    };
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!_stricmp(s, known[i]))
            return 1;
    return 0;
}

static int parse_bind(WmConfig *cfg, const char *val)
{
    char buf[512];
    char *part[4] = { 0 };
    int n = 0;
    strncpy(buf, val, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *p = buf;
    while (n < 4) {
        part[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = 0;
        p = comma + 1;
    }
    if (n < 3)
        return 0;
    for (int i = 0; i < n; i++)
        ntu_trim(part[i]);
    unsigned mods, vk;
    if (!wm_parse_mods(part[0], &mods) || !wm_parse_key(part[1], &vk) ||
        !action_known(part[2]))
        return 0;
    bind_add(cfg, mods, vk, part[2], n >= 4 ? part[3] : "");
    return 1;
}

static int glob_match_ci(const char *pat, const char *s)
{
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            while (*s)
                if (glob_match_ci(pat, s++))
                    return 1;
            return 0;
        }
        if (*pat == '?') {
            if (!*s) return 0;
            pat++; s++;
            continue;
        }
        if (tolower((unsigned char)*pat) != tolower((unsigned char)*s))
            return 0;
        pat++; s++;
    }
    return *s == 0;
}

void wm_apply_rules(Client *c)
{
    if (!c)
        return;
    int workspace = c->ws;
    int floating = c->floating;
    WmStyle style = c->style;
    for (int i = 0; i < g_wm.config.nrules; i++) {
        WmRule *r = &g_wm.config.rules[i];
        if (r->match_kind >= 0 && r->match_kind != c->kind)
            continue;
        if (r->match_workspace >= 0 && r->match_workspace != c->ws)
            continue;
        if (r->title_glob[0] && !glob_match_ci(r->title_glob, c->title))
            continue;
        if (r->exe_glob[0] && !glob_match_ci(r->exe_glob, c->exe))
            continue;
        if (r->effects & RULE_FLOATING)  floating = r->floating;
        if (r->effects & RULE_WORKSPACE) workspace = r->workspace;
        if (r->effects & RULE_BORDER)    style.border = r->border;
        if (r->effects & RULE_OPACITY)   style.opacity = r->opacity;
        if (r->effects & RULE_SHADOW)    style.shadow = r->shadow;
        if (r->effects & RULE_ANIMATE)   style.animate = r->animate;
        if (r->effects & RULE_TITLEBAR)  style.titlebar = r->titlebar;
    }
    wm_client_set_floating(c, floating);
    c->style = style;
    if (workspace != c->ws)
        wm_client_set_workspace(c, workspace);
}

static int parse_rule_kind(const char *s, int *kind)
{
    if (!_stricmp(s, "any"))      { *kind = -1; return 1; }
    if (!_stricmp(s, "terminal")) { *kind = 0; return 1; }
    if (!_stricmp(s, "app"))      { *kind = 1; return 1; }
    if (!_stricmp(s, "win32") || !_stricmp(s, "foreign")) {
        *kind = 2; return 1;
    }
    return 0;
}

static int section_workspace(const char *section)
{
    if (_strnicmp(section, "workspace ", 10))
        return -1;
    int n;
    if (!parse_long_strict(section + 10, 1, NTUWM_WS, &n))
        return -1;
    return n - 1;
}

static WmRule *section_rule(WmConfig *cfg, const char *section)
{
    if (_strnicmp(section, "rule ", 5) || !section[5])
        return NULL;
    const char *name = section + 5;
    for (int i = 0; i < cfg->nrules; i++)
        if (!_stricmp(cfg->rules[i].name, name))
            return &cfg->rules[i];
    if (cfg->nrules >= WM_MAX_RULES)
        return NULL;
    if (strlen(name) >= sizeof cfg->rules[0].name)
        return NULL;
    WmRule *r = &cfg->rules[cfg->nrules++];
    ZeroMemory(r, sizeof *r);
    strcpy(r->name, name);
    r->match_kind = -1;
    r->match_workspace = -1;
    r->workspace = -1;
    return r;
}

static int set_value(WmConfig *cfg, const char *sec, const char *key,
                     const char *val)
{
    int iv;
    float fv;
    if (!sec[0] || !_stricmp(sec, "ntwm")) {
        if (!_stricmp(key, "mod")) return set_base_mod(cfg, val);
        if (!_stricmp(key, "nmaster")) return parse_long_strict(val, 0, 32, &cfg->nmaster);
        if (!_stricmp(key, "mfact")) return parse_float_strict(val, 0.10f, 0.90f, &cfg->mfact);
        if (!_stricmp(key, "gap")) {
            if (!parse_long_strict(val, 0, 200, &iv)) return 0;
            cfg->gap_inner = cfg->gap_outer = iv; return 1;
        }
        if (!_stricmp(key, "border")) return parse_long_strict(val, 0, 32, &cfg->style.border);
        return 0;
    }
    if (!_stricmp(sec, "general")) {
        if (!_stricmp(key, "mod")) return set_base_mod(cfg, val);
        if (!_stricmp(key, "layout")) return parse_layout(val, &cfg->default_layout);
        if (!_stricmp(key, "gaps_in")) return parse_long_strict(val, 0, 200, &cfg->gap_inner);
        if (!_stricmp(key, "gaps_out")) return parse_long_strict(val, 0, 200, &cfg->gap_outer);
        if (!_stricmp(key, "drag_threshold"))
            return parse_long_strict(val, 0, 64, &cfg->drag_threshold);
        return 0;
    }
    if (!_stricmp(sec, "decoration")) {
        if (!_stricmp(key, "border_size")) return parse_long_strict(val, 0, 32, &cfg->style.border);
        if (!_stricmp(key, "active_border")) return parse_color(val, &cfg->style.border_rgb);
        if (!_stricmp(key, "opacity")) return parse_long_strict(val, 0, 255, &cfg->style.opacity);
        if (!_stricmp(key, "shadow")) return parse_bool(val, &cfg->style.shadow);
        if (!_stricmp(key, "rounding")) return parse_long_strict(val, 0, 64, &cfg->style.radius);
        if (!_stricmp(key, "titlebar")) return parse_bool(val, &cfg->style.titlebar);
        return 0;
    }
    if (!_stricmp(sec, "animations")) {
        if (!_stricmp(key, "enabled")) return parse_bool(val, &cfg->animations);
        if (!_stricmp(key, "move_ms")) return parse_long_strict(val, 0, 5000, &cfg->move_ms);
        if (!_stricmp(key, "open_ms")) return parse_long_strict(val, 0, 5000, &cfg->open_ms);
        if (!_stricmp(key, "workspace_ms")) return parse_long_strict(val, 0, 5000, &cfg->workspace_ms);
        if (!_stricmp(key, "focus_ms")) return parse_long_strict(val, 0, 5000, &cfg->focus_ms);
        return 0;
    }
    if (!_stricmp(sec, "dwindle")) {
        if (!_stricmp(key, "default_split_ratio"))
            return parse_long_strict(val, 10, 90, &cfg->dwindle_default_ratio);
        return 0;
    }
    if (!_stricmp(sec, "master")) {
        if (!_stricmp(key, "orientation")) return parse_orientation(val, &cfg->master_orientation);
        if (!_stricmp(key, "nmaster")) return parse_long_strict(val, 0, 32, &cfg->nmaster);
        if (!_stricmp(key, "mfact")) return parse_float_strict(val, 0.10f, 0.90f, &cfg->mfact);
        if (!_stricmp(key, "new_on_top") || !_stricmp(key, "insert_master"))
            return parse_bool(val, &cfg->insert_master);
        return 0;
    }
    if (!_stricmp(sec, "binds")) {
        if (_stricmp(key, "bind"))
            return 0;
        return parse_bind(cfg, val);
    }
    if (!_stricmp(sec, "bar")) {
        if (!_stricmp(key, "enabled")) return parse_bool(val, &cfg->bar_enabled);
        if (!_stricmp(key, "height")) return parse_long_strict(val, 20, 128, &cfg->bar_height);
        return 0;
    }
    int wi = section_workspace(sec);
    if (wi >= 0) {
        if (!_stricmp(key, "layout")) {
            cfg->workspace[wi].set_layout = 1;
            return parse_layout(val, &cfg->workspace[wi].layout);
        }
        if (!_stricmp(key, "nmaster")) {
            cfg->workspace[wi].set_nmaster = 1;
            return parse_long_strict(val, 0, 32, &cfg->workspace[wi].nmaster);
        }
        if (!_stricmp(key, "mfact")) {
            cfg->workspace[wi].set_mfact = 1;
            return parse_float_strict(val, 0.10f, 0.90f, &cfg->workspace[wi].mfact);
        }
        if (!_stricmp(key, "orientation")) {
            cfg->workspace[wi].set_orientation = 1;
            return parse_orientation(val, &cfg->workspace[wi].orientation);
        }
        if (!_stricmp(key, "name")) {
            if (!*val || strlen(val) >= sizeof cfg->workspace[wi].name) return 0;
            strcpy(cfg->workspace[wi].name, val); return 1;
        }
        return 0;
    }
    WmRule *r = section_rule(cfg, sec);
    if (r) {
        if (!_stricmp(key, "kind")) return parse_rule_kind(val, &r->match_kind);
        if (!_stricmp(key, "title_glob")) {
            if (strlen(val) >= sizeof r->title_glob) return 0;
            strcpy(r->title_glob, val); return 1;
        }
        if (!_stricmp(key, "exe_glob")) {
            if (strlen(val) >= sizeof r->exe_glob) return 0;
            strcpy(r->exe_glob, val); return 1;
        }
        if (!_stricmp(key, "match_workspace")) {
            if (!parse_long_strict(val, 1, NTUWM_WS, &iv)) return 0;
            r->match_workspace = iv - 1; return 1;
        }
        if (!_stricmp(key, "floating")) {
            if (!parse_bool(val, &r->floating)) return 0;
            r->effects |= RULE_FLOATING; return 1;
        }
        if (!_stricmp(key, "workspace")) {
            if (!parse_long_strict(val, 1, NTUWM_WS, &iv)) return 0;
            r->workspace = iv - 1; r->effects |= RULE_WORKSPACE; return 1;
        }
        if (!_stricmp(key, "border")) {
            if (!parse_long_strict(val, 0, 32, &r->border)) return 0;
            r->effects |= RULE_BORDER; return 1;
        }
        if (!_stricmp(key, "opacity")) {
            if (!parse_long_strict(val, 0, 255, &r->opacity)) return 0;
            r->effects |= RULE_OPACITY; return 1;
        }
        if (!_stricmp(key, "shadow")) {
            if (!parse_bool(val, &r->shadow)) return 0;
            r->effects |= RULE_SHADOW; return 1;
        }
        if (!_stricmp(key, "animation")) {
            if (!parse_bool(val, &r->animate)) return 0;
            r->effects |= RULE_ANIMATE; return 1;
        }
        if (!_stricmp(key, "titlebar")) {
            if (!parse_bool(val, &r->titlebar)) return 0;
            r->effects |= RULE_TITLEBAR; return 1;
        }
        return 0;
    }
    (void)fv;
    return 0;
}

static int section_valid(const char *sec)
{
    if (!sec[0] || !_stricmp(sec, "ntwm") || !_stricmp(sec, "general") ||
        !_stricmp(sec, "decoration") || !_stricmp(sec, "animations") ||
        !_stricmp(sec, "dwindle") || !_stricmp(sec, "master") ||
        !_stricmp(sec, "binds") || !_stricmp(sec, "bar"))
        return 1;
    if (section_workspace(sec) >= 0)
        return 1;
    return !_strnicmp(sec, "rule ", 5) && sec[5];
}

int wm_config_load(WmConfig *out, char *error, size_t error_cap)
{
    char path[MAX_PATH];
    ntu_path("/etc/ntwm/ntwm.conf", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f)
        return cfg_error(error, error_cap, 0, "nao foi possivel abrir o arquivo");
    WmConfig tmp;
    wm_config_defaults(&tmp);
    char sec[128] = "";
    char line[1024];
    int lineno = 0;
    int binds_cleared = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF);
            fclose(f);
            return cfg_error(error, error_cap, lineno, "linha maior que 1023 bytes");
        }
        ntu_trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';')
            continue;
        if (line[0] == '[') {
            char *end = strrchr(line, ']');
            if (!end || end[1]) {
                fclose(f);
                return cfg_error(error, error_cap, lineno, "secao malformada");
            }
            *end = 0;
            strncpy(sec, line + 1, sizeof sec - 1);
            sec[sizeof sec - 1] = 0;
            ntu_trim(sec);
            if (!section_valid(sec)) {
                fclose(f);
                return cfg_error(error, error_cap, lineno, "secao desconhecida [%s]", sec);
            }
            if (!_stricmp(sec, "binds") && !binds_cleared) {
                tmp.nbinds = 0; /* uma secao [binds] explicita substitui defaults */
                binds_cleared = 1;
            }
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            fclose(f);
            return cfg_error(error, error_cap, lineno, "esperado chave=valor");
        }
        *eq = 0;
        char *key = line, *val = eq + 1;
        ntu_trim(key); ntu_trim(val);
        if (!*key || !*val || !set_value(&tmp, sec, key, val)) {
            fclose(f);
            return cfg_error(error, error_cap, lineno,
                             "valor ou chave invalida: %s", key);
        }
    }
    if (ferror(f)) {
        fclose(f);
        return cfg_error(error, error_cap, lineno, "erro de leitura");
    }
    fclose(f);
    *out = tmp;
    return 1;
}

int wm_reload_config(int initial)
{
    WmConfig next;
    char error[320];
    if (!wm_config_load(&next, error, sizeof error)) {
        wm_report("%s", error);
        wm_ipc_event("config-error", error);
        return 0;
    }
    g_wm.config = next;
    if (!initial) {
        for (int i = 0; i < NTUWM_WS; i++) {
            Workspace *ws = &g_wm.workspaces[i];
            ws->gap_inner = next.gap_inner;
            ws->gap_outer = next.gap_outer;
            ws->insert_master = next.insert_master;
            wm_workspace_set_layout(ws, next.workspace[i].set_layout
                ? next.workspace[i].layout : next.default_layout);
            ws->nmaster = next.workspace[i].set_nmaster
                ? next.workspace[i].nmaster : next.nmaster;
            ws->mfact = next.workspace[i].set_mfact
                ? next.workspace[i].mfact : next.mfact;
            ws->orientation = next.workspace[i].set_orientation
                ? next.workspace[i].orientation : next.master_orientation;
            if (next.workspace[i].name[0])
                strncpy(ws->name, next.workspace[i].name, sizeof ws->name - 1);
        }
        for (Client *c = g_wm.clients; c; c = c->next) {
            c->style = next.style;
            wm_apply_rules(c);
        }
        wm_register_grabs();
        wm_request_frame();
        wm_report("configuracao recarregada");
        wm_ipc_event("reload", "ok");
    }
    return 1;
}
