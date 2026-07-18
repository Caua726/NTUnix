/*
 * service.c — tabela de serviços, spawn em Job Objects e supervisão.
 *
 * Cada serviço roda dentro de um Job Object próprio com
 * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: se o initd morrer, a árvore inteira
 * do serviço morre junto; parar um serviço = TerminateJobObject (mata a
 * árvore, ver VISAO.md §11). Um watcher thread por instância detecta a
 * saída e aplica a política Restart= com throttle (5 restarts / 10 s).
 */
#include "initd.h"

CRITICAL_SECTION g_lock;
Service *g_services = NULL;

static DWORD WINAPI watcher_main(LPVOID arg);

/* ---------------- parsing de units ---------------- */

static unsigned long long parse_size(const char *v)
{
    char *end;
    unsigned long long n = strtoull(v, &end, 10);
    if (*end == 'K' || *end == 'k') n <<= 10;
    else if (*end == 'M' || *end == 'm') n <<= 20;
    else if (*end == 'G' || *end == 'g') n <<= 30;
    return n;
}

static void add_deps(Service *s, const char *value)
{
    char buf[512];
    strncpy(buf, value, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, " ,\t"); tok; tok = strtok(NULL, " ,\t")) {
        char name[64];
        strncpy(name, tok, sizeof name - 1);
        name[sizeof name - 1] = 0;
        char *dot = strrchr(name, '.');
        if (dot && !_stricmp(dot, ".service"))
            *dot = 0; /* "logd.service" → "logd" */
        if (s->nrequires < MAX_DEPS) {
            strncpy(s->requires_[s->nrequires], name, 63);
            s->requires_[s->nrequires][63] = 0;
            s->nrequires++;
        }
    }
}

static void unit_kv(const char *sec, const char *key, const char *val, void *ud)
{
    Service *s = ud;
    if (!_stricmp(sec, "Unit")) {
        if (!_stricmp(key, "Description")) {
            strncpy(s->description, val, sizeof s->description - 1);
        } else if (!_stricmp(key, "Requires") || !_stricmp(key, "After")) {
            /* v0: After= tratado como Requires= (so ordenacao de start) */
            add_deps(s, val);
        }
    } else if (!_stricmp(sec, "Service")) {
        if (!_stricmp(key, "ExecStart"))
            strncpy(s->exec_start, val, sizeof s->exec_start - 1);
        else if (!_stricmp(key, "WorkingDirectory"))
            strncpy(s->workdir, val, sizeof s->workdir - 1);
        else if (!_stricmp(key, "Restart")) {
            if (!_stricmp(val, "always")) s->restart = RS_ALWAYS;
            else if (!_stricmp(val, "on-failure")) s->restart = RS_ON_FAILURE;
            else s->restart = RS_NO;
        } else if (!_stricmp(key, "MemoryMax"))
            s->memory_max = parse_size(val);
    }
    /* [Install] ignorado na v0: enable = marcador em /etc/units/enabled */
}

static void enabled_marker_path(const char *name, char *out, size_t cap)
{
    char rel[160];
    snprintf(rel, sizeof rel, "/etc/units/enabled/%s", name);
    ntu_path(rel, out, cap);
}

static int is_enabled(const char *name)
{
    char p[MAX_PATH];
    enabled_marker_path(name, p, sizeof p);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

Service *svc_find(const char *name)
{
    for (Service *s = g_services; s; s = s->next)
        if (!_stricmp(s->name, name))
            return s;
    return NULL;
}

/* copia so a parte de configuracao (preserva estado de runtime) */
static void copy_config(Service *dst, const Service *src)
{
    memcpy(dst->description, src->description, sizeof dst->description);
    memcpy(dst->exec_start, src->exec_start, sizeof dst->exec_start);
    memcpy(dst->workdir, src->workdir, sizeof dst->workdir);
    memcpy(dst->requires_, src->requires_, sizeof dst->requires_);
    dst->nrequires = src->nrequires;
    dst->restart = src->restart;
    dst->memory_max = src->memory_max;
}

int svc_scan_units(void)
{
    char dir[MAX_PATH], pat[MAX_PATH];
    ntu_path("/etc/units", dir, sizeof dir);
    snprintf(pat, sizeof pat, "%s\\*.service", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    int count = 0;
    do {
        Service parsed;
        memset(&parsed, 0, sizeof parsed);

        strncpy(parsed.name, fd.cFileName, sizeof parsed.name - 1);
        char *dot = strrchr(parsed.name, '.');
        if (dot) *dot = 0;
        snprintf(parsed.unit_path, sizeof parsed.unit_path, "%s\\%s", dir, fd.cFileName);

        if (ntu_ini_parse(parsed.unit_path, unit_kv, &parsed) != 0) {
            ilog("%s: falha ao ler %s", parsed.name, parsed.unit_path);
            continue;
        }
        if (!parsed.exec_start[0]) {
            ilog("%s: unit sem ExecStart, ignorada", parsed.name);
            continue;
        }

        EnterCriticalSection(&g_lock);
        Service *s = svc_find(parsed.name);
        if (!s) {
            s = calloc(1, sizeof *s);
            if (!s) {   /* audit #77: OOM -> memcpy(NULL) crasharia; ignora a unit */
                LeaveCriticalSection(&g_lock);
                ilog("%s: OOM ao registrar servico, ignorada", parsed.name);
                continue;
            }
            memcpy(s, &parsed, sizeof *s);
            s->state = ST_STOPPED;
            s->next = g_services;
            g_services = s;
            count++;
        } else if (s->state != ST_RUNNING) {
            /* recarrega config de servico parado; unit de servico rodando
             * so e aplicada no proximo restart (v0: nem isso — skip) */
            copy_config(s, &parsed);
            strncpy(s->unit_path, parsed.unit_path, sizeof s->unit_path - 1);
        }
        s->enabled = is_enabled(s->name);
        LeaveCriticalSection(&g_lock);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}

/* ---------------- spawn / stop ---------------- */

/* chamador segura g_lock */
static int svc_spawn(Service *s, char *err, size_t errcap)
{
    /* separa programa e argumentos (v0: sem quoting, split no 1o espaco) */
    char prog[512] = "", rest[512] = "";
    strncpy(prog, s->exec_start, sizeof prog - 1);
    char *sp = strchr(prog, ' ');
    if (sp) {
        *sp = 0;
        strncpy(rest, sp + 1, sizeof rest - 1);
        ntu_trim(rest);
    }
    char wprog[MAX_PATH];
    ntu_path(prog, wprog, sizeof wprog);

    char cmd[1200];
    if (rest[0])
        snprintf(cmd, sizeof cmd, "\"%s\" %s", wprog, rest);
    else
        snprintf(cmd, sizeof cmd, "\"%s\"", wprog);

    /* stdout/stderr do servico vao para /var/log/<nome>.log */
    char rel[160], logp[MAX_PATH];
    snprintf(rel, sizeof rel, "/var/log/%s.log", s->name);
    ntu_path(rel, logp, sizeof logp);

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE hlog = CreateFileA(logp, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hlog == INVALID_HANDLE_VALUE) {
        snprintf(err, errcap, "nao consegui abrir log %s (%lu)", logp, GetLastError());
        return -1;
    }

    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (!job) {
        snprintf(err, errcap, "CreateJobObject falhou (%lu)", GetLastError());
        CloseHandle(hlog);
        return -1;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    ZeroMemory(&jeli, sizeof jeli);
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (s->memory_max) {
        jeli.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        jeli.JobMemoryLimit = (SIZE_T)s->memory_max;
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &jeli, sizeof jeli)) {
        /* Wine antigo pode nao suportar limite de memoria — tenta sem */
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        jeli.JobMemoryLimit = 0;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &jeli, sizeof jeli)) {
            /* audit #72: sem KILL_ON_JOB_CLOSE os filhos escapam do kill
             * coletivo (viram orfaos) -> aborta em vez de job sem contencao */
            snprintf(err, errcap, "SetInformationJobObject falhou (%lu)",
                     GetLastError());
            CloseHandle(hlog);
            CloseHandle(job);
            return -1;
        }
        ilog("%s: MemoryMax nao suportado aqui, seguindo sem limite", s->name);
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hlog;
    si.hStdError = hlog;

    char wdir[MAX_PATH];
    if (s->workdir[0])
        ntu_path(s->workdir, wdir, sizeof wdir);
    else {
        strncpy(wdir, ntu_root(), sizeof wdir - 1);
        wdir[sizeof wdir - 1] = 0;
    }

    /* filhos herdam a raiz NTUnix */
    SetEnvironmentVariableA("NTUNIX_ROOT", ntu_root());

    PROCESS_INFORMATION pi;
    /* suspenso ate entrar no job — evita filho escapar do kill coletivo */
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW,
                             NULL, wdir, &si, &pi);
    CloseHandle(hlog);
    if (!ok) {
        snprintf(err, errcap, "CreateProcess falhou (%lu): %s", GetLastError(), cmd);
        CloseHandle(job);
        return -1;
    }
    /* se a atribuicao ao job falhar, o filho escaparia do MemoryMax e do kill
     * coletivo (KILL_ON_JOB_CLOSE) — aborta em vez de seguir sem contencao (#133) */
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        snprintf(err, errcap, "AssignProcessToJobObject falhou (%lu): %s",
                 GetLastError(), cmd);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return -1;
    }
    /* audit #71: ResumeThread falho deixaria o filho SUSPENSO pra sempre
     * (servico "running" mas congelado, watcher esperando eternamente) —
     * aborta e limpa job/processo. */
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        snprintf(err, errcap, "ResumeThread falhou (%lu): %s", GetLastError(), cmd);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return -1;
    }
    CloseHandle(pi.hThread);

    s->job = job;
    s->process = pi.hProcess;
    s->pid = pi.dwProcessId;
    s->state = ST_RUNNING;
    s->stopping = FALSE;
    s->started_at = GetTickCount64();

    HANDLE t = CreateThread(NULL, 0, watcher_main, s, 0, NULL);
    if (t) CloseHandle(t);
    return 0;
}

static DWORD WINAPI watcher_main(LPVOID arg)
{
    Service *s = arg;

    EnterCriticalSection(&g_lock);
    HANDLE proc = s->process;
    LeaveCriticalSection(&g_lock);
    if (!proc)
        return 0;

    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);

    BOOL do_restart = FALSE;
    EnterCriticalSection(&g_lock);
    if (s->process != proc) {
        /* instancia ja substituida; este watcher e obsoleto */
        LeaveCriticalSection(&g_lock);
        CloseHandle(proc);
        return 0;
    }
    CloseHandle(s->process);
    s->process = NULL;
    if (s->job) { CloseHandle(s->job); s->job = NULL; }
    s->pid = 0;
    s->last_exit = code;

    if (s->stopping || g_shutdown) {
        s->state = ST_STOPPED;
        ilog("%s: parado (exit=%lu)", s->name, code);
    } else if (s->restart == RS_ALWAYS ||
               (s->restart == RS_ON_FAILURE && code != 0)) {
        ULONGLONG now = GetTickCount64();
        if (now - s->win_start > 10000) {
            s->win_start = now;
            s->win_restarts = 0;
        }
        if (++s->win_restarts > 5) {
            s->state = ST_FAILED;
            ilog("%s: %d saidas em 10s, desistindo (exit=%lu)",
                 s->name, s->win_restarts, code);
        } else {
            s->state = ST_STOPPED;
            do_restart = TRUE;
            ilog("%s: saiu (exit=%lu), reiniciando em 1s", s->name, code);
        }
    } else {
        s->state = (code == 0) ? ST_STOPPED : ST_FAILED;
        ilog("%s: saiu (exit=%lu)", s->name, code);
    }
    LeaveCriticalSection(&g_lock);

    if (do_restart) {
        Sleep(1000);
        EnterCriticalSection(&g_lock);
        if (!s->stopping && !g_shutdown && s->state == ST_STOPPED) {
            char err[256];
            s->restarts++;
            if (svc_spawn(s, err, sizeof err) != 0) {
                s->state = ST_FAILED;
                ilog("%s: restart falhou: %s", s->name, err);
            }
        }
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

/* ---------------- API usada pelo pipe server ---------------- */

static int start_with_deps(Service *s, int depth, char *err, size_t errcap)
{
    if (depth > MAX_DEPS) {
        snprintf(err, errcap, "ciclo de dependencias envolvendo %s", s->name);
        return -1;
    }
    for (int i = 0; i < s->nrequires; i++) {
        Service *dep = svc_find(s->requires_[i]);
        if (!dep) {
            ilog("%s: dependencia desconhecida '%s' (ignorada)",
                 s->name, s->requires_[i]);
            continue;
        }
        if (start_with_deps(dep, depth + 1, err, errcap) != 0)
            return -1;
    }
    EnterCriticalSection(&g_lock);
    int rc = 0;
    if (s->state != ST_RUNNING) {
        s->stopping = FALSE;
        rc = svc_spawn(s, err, errcap);
        if (rc == 0)
            ilog("%s: iniciado (pid %lu)", s->name, s->pid);
    }
    LeaveCriticalSection(&g_lock);
    return rc;
}

int svc_start(Service *s, char *err, size_t errcap)
{
    return start_with_deps(s, 0, err, errcap);
}

int svc_stop(Service *s)
{
    EnterCriticalSection(&g_lock);
    if (s->state != ST_RUNNING) {
        s->state = ST_STOPPED; /* stop em failed limpa o estado */
        LeaveCriticalSection(&g_lock);
        return 0;
    }
    s->stopping = TRUE;
    TerminateJobObject(s->job, 1); /* mata a arvore toda do servico */
    LeaveCriticalSection(&g_lock);
    return 0;
}

int svc_wait_stopped(Service *s, DWORD timeout_ms)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        EnterCriticalSection(&g_lock);
        SvcState st = s->state;
        LeaveCriticalSection(&g_lock);
        if (st != ST_RUNNING)
            return 0;
        if (GetTickCount64() >= deadline)
            return -1;
        Sleep(50);
    }
}

void svc_stop_all(void)
{
    for (Service *s = g_services; s; s = s->next)
        svc_stop(s);
    for (Service *s = g_services; s; s = s->next)
        svc_wait_stopped(s, 3000);
}

int svc_set_enabled(Service *s, int on)
{
    char p[MAX_PATH];
    enabled_marker_path(s->name, p, sizeof p);
    if (on) {
        HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE)
            return -1;
        CloseHandle(h);
    } else {
        if (!DeleteFileA(p) && GetLastError() != ERROR_FILE_NOT_FOUND)
            return -1;
    }
    s->enabled = on;
    return 0;
}
