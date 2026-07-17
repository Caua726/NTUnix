/* initd.h — estado interno do supervisor NTUnix. */
#ifndef INITD_H
#define INITD_H

#include "../common/ntu.h"

typedef enum { ST_STOPPED, ST_RUNNING, ST_FAILED } SvcState;
typedef enum { RS_NO, RS_ON_FAILURE, RS_ALWAYS } RestartPolicy;

#define MAX_DEPS 8

typedef struct Service {
    /* identidade + config (do arquivo .service) */
    char name[64];
    char unit_path[MAX_PATH];
    char description[256];
    char exec_start[512];
    char workdir[512];
    char requires_[MAX_DEPS][64];
    int nrequires;
    RestartPolicy restart;
    unsigned long long memory_max; /* bytes; 0 = sem limite */
    int enabled;

    /* runtime (protegido por g_lock) */
    SvcState state;
    HANDLE job;      /* Job Object com KILL_ON_JOB_CLOSE — cgroup do pobre */
    HANDLE process;
    DWORD pid;
    BOOL stopping;   /* parada explicita: watcher nao deve reiniciar */
    int restarts;    /* total desde o boot do initd */
    int win_restarts;
    ULONGLONG win_start; /* janela do throttle de restart */
    DWORD last_exit;
    ULONGLONG started_at;

    struct Service *next;
} Service;

extern CRITICAL_SECTION g_lock;
extern Service *g_services;
extern volatile LONG g_shutdown;

/* initd.c */
void ilog(const char *fmt, ...);

/* service.c */
int svc_scan_units(void);            /* carrega/atualiza units de /etc/units */
Service *svc_find(const char *name); /* sem lock — lista so cresce           */
int svc_start(Service *s, char *err, size_t errcap); /* com dependencias     */
int svc_stop(Service *s);
int svc_wait_stopped(Service *s, DWORD timeout_ms);
void svc_stop_all(void);
int svc_set_enabled(Service *s, int on);

/* pipesrv.c */
void pipe_server_run(void);

#endif
