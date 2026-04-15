/*
 * engine.c — Supervised Multi-Container Runtime (User Space)
 *
 * Full implementation:
 *   - UNIX-socket control plane (Path B)
 *   - pipe-based log capture with bounded-buffer producer/consumer (Path A)
 *   - clone() with PID / UTS / mount namespace isolation
 *   - chroot + /proc mount inside each container
 *   - signalfd-based SIGCHLD / SIGINT / SIGTERM handling
 *   - kernel-module registration via ioctl (optional if module not loaded)
 *   - CMD_RUN blocking with per-container condition variable
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ─── constants ─────────────────────────────────────────────────── */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CTRL_MSG_LEN        4096
#define CHILD_CMD_LEN       256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT  (64UL << 20)   /* 64 MiB */
#define MONITOR_DEV         "/dev/container_monitor"

/* ─── enums ──────────────────────────────────────────────────────── */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ─── structs ────────────────────────────────────────────────────── */

typedef struct container_record {
    char                  id[CONTAINER_ID_LEN];
    pid_t                 host_pid;
    time_t                started_at;
    container_state_t     state;
    unsigned long         soft_limit_bytes;
    unsigned long         hard_limit_bytes;
    int                   exit_code;
    int                   exit_signal;
    char                  log_path[PATH_MAX];

    /* stop-request flag for termination attribution (Task 4) */
    int                   stop_requested;

    /* logging pipeline */
    int                   pipe_read_fd;
    pthread_t             producer_tid;
    int                   producer_started;

    /* CMD_RUN blocking: handler thread waits on run_cond */
    pthread_mutex_t       run_mutex;
    pthread_cond_t        run_cond;
    int                   run_done;

    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head, tail, count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty, not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_CMD_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;                 /* 0=ok, -1=error, 1=more-data */
    char message[CTRL_MSG_LEN];
} control_response_t;

/* Passed through clone() into child_fn — lives on the heap */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_CMD_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

/* Arg for each per-container producer thread */
typedef struct {
    int             pipe_fd;
    char            container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_arg_t;

/* Supervisor state (one instance per process lifetime) */
typedef struct {
    int                  server_fd;
    int                  monitor_fd;
    volatile int         should_stop;
    pthread_t            logger_tid;
    bounded_buffer_t     log_buffer;
    pthread_mutex_t      metadata_lock;
    container_record_t  *containers;
} supervisor_ctx_t;

/* Arg passed to each client-handler thread */
typedef struct {
    int              client_fd;
    supervisor_ctx_t *ctx;
} client_arg_t;

/* ─── forward declarations ──────────────────────────────────────── */
static int send_control_request(const control_request_t *req);

/* ═══════════════════════════════════════════════════════════════════
 * USAGE / PARSE HELPERS
 * ═══════════════════════════════════════════════════════════════════ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>\n"
        "  %s start <id> <container-rootfs> <command>"
            " [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run   <id> <container-rootfs> <command>"
            " [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs <id>\n"
        "  %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value,
                           unsigned long *out)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno || end == value || *end) {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    *out = mib << 20;
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                 int argc, char *argv[], int start)
{
    int i;
    for (i = start; i < argc; i += 2) {
        char *end;
        long nv;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1],
                               &req->soft_limit_bytes))
                return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1],
                               &req->hard_limit_bytes))
                return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nv = strtol(argv[i+1], &end, 10);
            if (errno || end == argv[i+1] || *end || nv < -20 || nv > 19) {
                fprintf(stderr,
                        "Invalid --nice value (expected -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nv;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * BOUNDED BUFFER
 *
 * Circular array protected by a mutex; producers block on not_full,
 * consumers block on not_empty.  Shutdown broadcasts both CVs so all
 * waiters can unblock and exit cleanly.
 * ═══════════════════════════════════════════════════════════════════ */

static int bounded_buffer_init(bounded_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/*
 * Push one log chunk.  Blocks if the buffer is full.
 * Returns 0 on success, -1 if shutdown was triggered before space opened.
 *
 * Race conditions without the mutex:
 *   - two producers could both observe count < capacity and double-insert,
 *     corrupting tail and count.
 *   - a producer could sleep on not_full while a consumer drains and
 *     signals, but the producer never sees the signal (lost wakeup).
 * The mutex + cond-var pair eliminates both.
 */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/*
 * Pop one log chunk.  Blocks while the buffer is empty and not shut down.
 * Returns 0 with *item filled, or -1 when the buffer is both empty and
 * shutting down (consumer should exit).
 *
 * Correctness guarantee: items already in the buffer when shutdown begins
 * are still returned before -1 is signalled, so no log line is dropped.
 */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0) {
        if (b->shutting_down) {
            pthread_mutex_unlock(&b->mutex);
            return -1;
        }
        pthread_cond_wait(&b->not_empty, &b->mutex);
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * LOGGER (CONSUMER) THREAD
 *
 * One thread for the whole supervisor.  Routes each log_item to the
 * correct per-container log file, opening files on first use.
 * Exits only when the buffer is both shutting_down and drained.
 * ═══════════════════════════════════════════════════════════════════ */

#define MAX_OPEN_LOG_FILES 64

static struct {
    char id[CONTAINER_ID_LEN];
    int  fd;
} g_log_fds[MAX_OPEN_LOG_FILES];
static int g_n_log_fds = 0;

static int get_or_open_log_fd(const char *container_id)
{
    int i, fd;
    char path[PATH_MAX];

    for (i = 0; i < g_n_log_fds; i++)
        if (strcmp(g_log_fds[i].id, container_id) == 0)
            return g_log_fds[i].fd;

    if (g_n_log_fds >= MAX_OPEN_LOG_FILES)
        return -1;

    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, container_id);
    fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0)
        return -1;

    strncpy(g_log_fds[g_n_log_fds].id, container_id,
            CONTAINER_ID_LEN - 1);
    g_log_fds[g_n_log_fds].fd = fd;
    g_n_log_fds++;
    return fd;
}

void *logging_thread(void *arg)
{
    bounded_buffer_t *buf = (bounded_buffer_t *)arg;
    log_item_t item;
    int i, fd;

    mkdir(LOG_DIR, 0755);

    /* Consume items until the buffer is drained and shut down */
    while (bounded_buffer_pop(buf, &item) == 0) {
        fd = get_or_open_log_fd(item.container_id);
        if (fd >= 0)
            (void)write(fd, item.data, item.length);
    }

    /* Drain any items that arrived right before shutdown */
    pthread_mutex_lock(&buf->mutex);
    while (buf->count > 0) {
        item = buf->items[buf->head];
        buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
        buf->count--;
        pthread_mutex_unlock(&buf->mutex);

        fd = get_or_open_log_fd(item.container_id);
        if (fd >= 0)
            (void)write(fd, item.data, item.length);

        pthread_mutex_lock(&buf->mutex);
    }
    pthread_mutex_unlock(&buf->mutex);

    /* Close all open log file descriptors */
    for (i = 0; i < g_n_log_fds; i++)
        close(g_log_fds[i].fd);

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * PRODUCER THREAD  (one per container)
 *
 * Reads container stdout/stderr from the pipe, packages into log_item_t
 * structs, and pushes them into the shared bounded buffer.
 * Exits cleanly when the pipe reaches EOF (container process exited).
 * ═══════════════════════════════════════════════════════════════════ */

static void *producer_thread_fn(void *arg)
{
    producer_arg_t *p = (producer_arg_t *)arg;
    char buf[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(p->pipe_fd, buf, sizeof(buf))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, p->container_id,
                CONTAINER_ID_LEN - 1);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        bounded_buffer_push(p->buffer, &item);
    }

    close(p->pipe_fd);
    free(p);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * CHILD FUNCTION  (runs inside new namespaces after clone())
 *
 * clone() with CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS gives the
 * child its own:
 *   - PID namespace  → child sees itself as PID 1
 *   - UTS namespace  → can set its own hostname
 *   - mount namespace→ can mount /proc without affecting the host
 *
 * chroot() restricts the filesystem view to rootfs.  We use chroot
 * rather than pivot_root for simplicity; CLONE_NEWNS prevents the
 * classic ".." escape because the container's mount table is private.
 * ═══════════════════════════════════════════════════════════════════ */

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char *argv[] = {"/bin/sh", "-c", cfg->command, NULL};
    int devnull;

    /* Set hostname to container ID (UTS namespace) */
    sethostname(cfg->id, strlen(cfg->id));

    /* Enter the container root filesystem */
    if (chroot(cfg->rootfs) < 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    /* Mount /proc so tools like 'ps' work inside the container.
     * mkdir is a best-effort precaution for minimal rootfs images. */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY) {
        /* Non-fatal: some rootfs images already have it mounted */
        fprintf(stderr, "warning: mount /proc: %s\n", strerror(errno));
    }

    /* Wire stdout and stderr to the supervisor's logging pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 log pipe");
        return 1;
    }
    if (cfg->log_write_fd > STDERR_FILENO)
        close(cfg->log_write_fd);

    /* Silence stdin */
    devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        close(devnull);
    }

    /* Apply scheduling priority before exec */
    if (cfg->nice_value != 0)
        setpriority(PRIO_PROCESS, 0, cfg->nice_value);

    /* Exec the requested command via the container's /bin/sh */
    execv("/bin/sh", argv);
    perror("execv");
    return 127;
}

/* ═══════════════════════════════════════════════════════════════════
 * KERNEL MONITOR  (ioctl helpers)
 * ═══════════════════════════════════════════════════════════════════ */

int register_with_monitor(int monitor_fd, const char *container_id,
                           pid_t host_pid,
                           unsigned long soft_limit_bytes,
                           unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, MONITOR_NAME_LEN - 1);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req);
}

int unregister_from_monitor(int monitor_fd, const char *container_id,
                             pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, MONITOR_NAME_LEN - 1);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}

/* ═══════════════════════════════════════════════════════════════════
 * CONTAINER METADATA HELPERS
 * ═══════════════════════════════════════════════════════════════════ */

/* Caller must hold metadata_lock */
static container_record_t *find_container_by_id(supervisor_ctx_t *ctx,
                                                  const char *id)
{
    container_record_t *c;
    for (c = ctx->containers; c; c = c->next)
        if (strcmp(c->id, id) == 0)
            return c;
    return NULL;
}

/* Caller must hold metadata_lock */
static container_record_t *find_container_by_pid(supervisor_ctx_t *ctx,
                                                   pid_t pid)
{
    container_record_t *c;
    for (c = ctx->containers; c; c = c->next)
        if (c->host_pid == pid)
            return c;
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * LAUNCH CONTAINER
 *
 * Creates a pipe, calls clone(), starts a producer thread, registers
 * the container with the kernel module, and inserts a record into the
 * supervisor's linked list.  Returns the host PID or -1 on failure.
 * ═══════════════════════════════════════════════════════════════════ */

static pid_t launch_container(supervisor_ctx_t *ctx,
                               const char *id,
                               const char *rootfs,
                               const char *command,
                               int nice_value,
                               unsigned long soft,
                               unsigned long hard)
{
    int pipefd[2];
    char *stack;
    pid_t pid;
    child_config_t *cfg;
    container_record_t *rec;
    producer_arg_t *parg;
    char log_path[PATH_MAX];

    /* Reject duplicate running container ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    {
        container_record_t *existing = find_container_by_id(ctx, id);
        if (existing && existing->state == CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            fprintf(stderr, "[supervisor] container '%s' already running\n",
                    id);
            return -1;
        }
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }

    /* Allocate child stack (grows down; pass top of buffer) */
    stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /* child_config_t on the heap so child can read it before exec */
    cfg = calloc(1, sizeof(child_config_t));
    if (!cfg) {
        free(stack);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    strncpy(cfg->id,      id,      CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  rootfs,  PATH_MAX - 1);
    strncpy(cfg->command, command, CHILD_CMD_LEN - 1);
    cfg->nice_value   = nice_value;
    cfg->log_write_fd = pipefd[1];

    /*
     * CLONE_NEWPID  — new PID namespace; child is PID 1 inside
     * CLONE_NEWUTS  — new UTS namespace; child can set hostname
     * CLONE_NEWNS   — new mount namespace; child can mount /proc
     * SIGCHLD       — deliver SIGCHLD to parent when child exits
     */
    pid = clone(child_fn, stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);

    if (pid < 0) {
        perror("clone");
        free(cfg);
        free(stack);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    /* Parent: close the write end — child owns it */
    close(pipefd[1]);
    /*
     * cfg was heap-allocated.  With CLONE_NEWNS (no CLONE_VM) the
     * child has its own address space copy, so the parent may free.
     * The child will read cfg before exec(), and exec() replaces its
     * address space entirely, so the parent's free() is safe here.
     */
    free(cfg);
    /* stack is intentionally not freed; the child's CoW copy is
     * released by the kernel on exec().  Leaking 1 MiB per container
     * is acceptable for this project scale. */
    (void)stack;

    /* Build log file path */
    mkdir(LOG_DIR, 0755);
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, id);

    /* Allocate and initialise container record */
    rec = calloc(1, sizeof(container_record_t));
    if (!rec) {
        kill(pid, SIGKILL);
        close(pipefd[0]);
        return -1;
    }
    strncpy(rec->id,  id,       CONTAINER_ID_LEN - 1);
    strncpy(rec->log_path, log_path, PATH_MAX - 1);
    rec->host_pid          = pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = soft;
    rec->hard_limit_bytes  = hard;
    rec->exit_code         = -1;
    rec->exit_signal       = 0;
    rec->stop_requested    = 0;
    rec->pipe_read_fd      = pipefd[0];
    rec->run_done          = 0;
    pthread_mutex_init(&rec->run_mutex, NULL);
    pthread_cond_init(&rec->run_cond, NULL);

    /* Prepend to container list */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Start per-container producer thread */
    parg = malloc(sizeof(producer_arg_t));
    if (parg) {
        parg->pipe_fd = pipefd[0];
        strncpy(parg->container_id, id, CONTAINER_ID_LEN - 1);
        parg->buffer = &ctx->log_buffer;
        if (pthread_create(&rec->producer_tid, NULL,
                           producer_thread_fn, parg) == 0) {
            rec->producer_started = 1;
            rec->pipe_read_fd = -1;  /* now owned by producer */
        } else {
            free(parg);
            close(pipefd[0]);
        }
    } else {
        close(pipefd[0]);
    }

    /* Register with kernel memory monitor (optional) */
    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd, id, pid, soft, hard) < 0)
            perror("[supervisor] ioctl MONITOR_REGISTER");
    }

    fprintf(stderr, "[supervisor] started container '%s' pid=%d\n",
            id, pid);
    return pid;
}

/* ═══════════════════════════════════════════════════════════════════
 * SIGCHLD REAPER
 *
 * Called from the signalfd path.  Reaps all exited children, updates
 * container state, unregisters from the kernel module, and wakes any
 * CMD_RUN waiter.
 *
 * Termination attribution (Task 4 requirement):
 *   - stop_requested set  →  CONTAINER_STOPPED
 *   - SIGKILL and no stop_requested  →  CONTAINER_KILLED (hard limit)
 *   - normal exit  →  CONTAINER_EXITED
 * ═══════════════════════════════════════════════════════════════════ */

static void reap_children(supervisor_ctx_t *ctx)
{
    int wstatus;
    pid_t pid;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        container_record_t *c;

        pthread_mutex_lock(&ctx->metadata_lock);
        c = find_container_by_pid(ctx, pid);
        if (c) {
            if (WIFEXITED(wstatus)) {
                c->exit_code   = WEXITSTATUS(wstatus);
                c->exit_signal = 0;
            } else if (WIFSIGNALED(wstatus)) {
                c->exit_code   = 128 + WTERMSIG(wstatus);
                c->exit_signal = WTERMSIG(wstatus);
            }

            if (c->stop_requested)
                c->state = CONTAINER_STOPPED;
            else if (WIFSIGNALED(wstatus) && WTERMSIG(wstatus) == SIGKILL)
                c->state = CONTAINER_KILLED;
            else
                c->state = CONTAINER_EXITED;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (c) {
            /* Unregister from kernel monitor */
            if (ctx->monitor_fd >= 0)
                unregister_from_monitor(ctx->monitor_fd, c->id, pid);

            /* Wake any CMD_RUN handler blocked on run_cond */
            pthread_mutex_lock(&c->run_mutex);
            c->run_done = 1;
            pthread_cond_signal(&c->run_cond);
            pthread_mutex_unlock(&c->run_mutex);

            fprintf(stderr,
                    "[supervisor] container '%s' pid=%d exited"
                    " state=%s exit_code=%d\n",
                    c->id, pid,
                    state_to_string(c->state), c->exit_code);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * CLIENT REQUEST HANDLER THREAD
 *
 * Each accepted connection is handled in its own detached thread so
 * that CMD_RUN can block indefinitely without stalling other clients.
 * ═══════════════════════════════════════════════════════════════════ */

static void send_response(int fd, int status, const char *msg)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = status;
    if (msg)
        strncpy(resp.message, msg, CTRL_MSG_LEN - 1);
    (void)write(fd, &resp, sizeof(resp));
}

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    char buf[CTRL_MSG_LEN];
    int pos = 0;
    container_record_t *c;
    char ts[24];

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "%-16s %-7s %-10s %-10s %-10s %-6s %s\n",
        "CONTAINER-ID", "PID", "STATE",
        "SOFT(MiB)", "HARD(MiB)", "EXIT", "STARTED");

    pthread_mutex_lock(&ctx->metadata_lock);
    for (c = ctx->containers;
         c && pos < (int)sizeof(buf) - 128;
         c = c->next) {
        struct tm *t = localtime(&c->started_at);
        strftime(ts, sizeof(ts), "%H:%M:%S", t);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%-16s %-7d %-10s %-10lu %-10lu %-6d %s\n",
            c->id, c->host_pid,
            state_to_string(c->state),
            c->soft_limit_bytes >> 20,
            c->hard_limit_bytes >> 20,
            c->exit_code, ts);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    send_response(client_fd, 0, buf);
}

static void handle_logs(supervisor_ctx_t *ctx, int client_fd,
                         const char *id)
{
    char path[PATH_MAX];
    char buf[CTRL_MSG_LEN - 32];
    ssize_t n;
    int fd;

    pthread_mutex_lock(&ctx->metadata_lock);
    {
        container_record_t *c = find_container_by_id(ctx, id);
        if (!c) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, -1, "Container not found");
            return;
        }
        strncpy(path, c->log_path, PATH_MAX - 1);
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        send_response(client_fd, 0,
                      "(log file not yet created — container may still be starting)\n");
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    send_response(client_fd, 0, buf);
}

static void handle_stop(supervisor_ctx_t *ctx, int client_fd,
                         const char *id)
{
    pid_t pid;

    pthread_mutex_lock(&ctx->metadata_lock);
    {
        container_record_t *c = find_container_by_id(ctx, id);
        if (!c) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, -1, "Container not found");
            return;
        }
        if (c->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, -1, "Container is not running");
            return;
        }
        c->stop_requested = 1;
        pid = c->host_pid;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    kill(pid, SIGTERM);
    send_response(client_fd, 0, "SIGTERM sent to container");
}

static void *client_handler_thread(void *arg)
{
    client_arg_t *ca = (client_arg_t *)arg;
    int fd = ca->client_fd;
    supervisor_ctx_t *ctx = ca->ctx;
    control_request_t req;

    free(ca);

    if (recv(fd, &req, sizeof(req), MSG_WAITALL) != (ssize_t)sizeof(req)) {
        close(fd);
        return NULL;
    }

    switch (req.kind) {

    case CMD_START: {
        pid_t pid = launch_container(ctx,
                                     req.container_id, req.rootfs,
                                     req.command, req.nice_value,
                                     req.soft_limit_bytes,
                                     req.hard_limit_bytes);
        if (pid < 0) {
            send_response(fd, -1, "Failed to start container");
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "Container '%s' started, pid=%d",
                     req.container_id, pid);
            send_response(fd, 0, msg);
        }
        break;
    }

    case CMD_RUN: {
        pid_t pid = launch_container(ctx,
                                     req.container_id, req.rootfs,
                                     req.command, req.nice_value,
                                     req.soft_limit_bytes,
                                     req.hard_limit_bytes);
        if (pid < 0) {
            send_response(fd, -1, "Failed to start container");
            break;
        }

        /* Locate the container record and block until it exits */
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container_by_pid(ctx, pid);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (c) {
            /*
             * Wait on the per-container run_cond.
             * reap_children() signals this when SIGCHLD fires for this PID.
             * We release run_mutex inside pthread_cond_wait, so the
             * SIGCHLD path can acquire it to signal us — no deadlock.
             */
            pthread_mutex_lock(&c->run_mutex);
            while (!c->run_done)
                pthread_cond_wait(&c->run_cond, &c->run_mutex);
            int exit_code = c->exit_code;
            container_state_t state = c->state;
            pthread_mutex_unlock(&c->run_mutex);

            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Container '%s' finished: state=%s exit_code=%d",
                     req.container_id, state_to_string(state), exit_code);
            /* Use exit_code as status so the shell can forward it */
            send_response(fd, exit_code, msg);
        } else {
            send_response(fd, -1, "Internal error: container record lost");
        }
        break;
    }

    case CMD_PS:
        handle_ps(ctx, fd);
        break;

    case CMD_LOGS:
        handle_logs(ctx, fd, req.container_id);
        break;

    case CMD_STOP:
        handle_stop(ctx, fd, req.container_id);
        break;

    default:
        send_response(fd, -1, "Unknown command");
        break;
    }

    close(fd);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════
 * SUPERVISOR  (long-running daemon)
 *
 * Event loop uses poll(2) over:
 *   pfds[0] = server UNIX socket  → accept new CLI connections
 *   pfds[1] = signalfd            → handle SIGCHLD / SIGINT / SIGTERM
 *
 * Signals are blocked via sigprocmask() so all threads inherit the
 * block; only the main thread drains the signalfd.
 * ═══════════════════════════════════════════════════════════════════ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    sigset_t mask;
    int sfd;   /* signalfd */
    struct pollfd pfds[2];
    (void)rootfs;   /* rootfs used for documentation / env-check only */

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;

    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);
    mkdir(LOG_DIR, 0755);

    /* Block SIGCHLD, SIGINT, SIGTERM — handled via signalfd */
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        perror("sigprocmask");
        return 1;
    }

    sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) {
        perror("signalfd");
        return 1;
    }

    /* Open kernel monitor device if the module is loaded */
    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] %s not found — memory monitoring disabled\n",
                MONITOR_DEV);

    /* Start logger (consumer) thread */
    if (pthread_create(&ctx.logger_tid, NULL,
                       logging_thread, &ctx.log_buffer) != 0) {
        perror("pthread_create logger");
        return 1;
    }

    /* Create UNIX domain socket */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }
    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen");
        return 1;
    }

    fprintf(stderr,
            "[supervisor] ready, socket=%s, monitor=%s\n",
            CONTROL_PATH,
            ctx.monitor_fd >= 0 ? MONITOR_DEV : "disabled");

    /* ── Event loop ─────────────────────────────────────────────── */
    pfds[0].fd     = ctx.server_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd     = sfd;
    pfds[1].events = POLLIN;

    while (!ctx.should_stop) {
        int ready = poll(pfds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        /* ── Signal event ───────────────────────────────────────── */
        if (pfds[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            ssize_t r = read(sfd, &si, sizeof(si));
            if (r == (ssize_t)sizeof(si)) {
                if (si.ssi_signo == SIGCHLD) {
                    reap_children(&ctx);
                } else {
                    fprintf(stderr,
                            "\n[supervisor] signal %u received — shutting down\n",
                            si.ssi_signo);
                    ctx.should_stop = 1;
                }
            }
        }

        /* ── New client connection ──────────────────────────────── */
        if (!ctx.should_stop && (pfds[0].revents & POLLIN)) {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0) {
                client_arg_t *ca = malloc(sizeof(client_arg_t));
                if (ca) {
                    pthread_t tid;
                    ca->client_fd = client_fd;
                    ca->ctx       = &ctx;
                    if (pthread_create(&tid, NULL,
                                       client_handler_thread, ca) == 0) {
                        pthread_detach(tid);
                    } else {
                        free(ca);
                        close(client_fd);
                    }
                } else {
                    close(client_fd);
                }
            }
        }
    }

    /* ── Graceful shutdown ──────────────────────────────────────── */
    fprintf(stderr, "[supervisor] stopping all containers...\n");

    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c;
        for (c = ctx.containers; c; c = c->next) {
            if (c->state == CONTAINER_RUNNING) {
                c->stop_requested = 1;
                kill(c->host_pid, SIGTERM);
            }
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Give containers 2 s to exit gracefully, then SIGKILL */
    sleep(2);
    {
        container_record_t *c;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (c = ctx.containers; c; c = c->next)
            if (c->state == CONTAINER_RUNNING)
                kill(c->host_pid, SIGKILL);
        pthread_mutex_unlock(&ctx.metadata_lock);
    }
    reap_children(&ctx);

    /* Join producer threads */
    {
        container_record_t *c;
        pthread_mutex_lock(&ctx.metadata_lock);
        for (c = ctx.containers; c; c = c->next) {
            if (c->producer_started) {
                pthread_mutex_unlock(&ctx.metadata_lock);
                pthread_join(c->producer_tid, NULL);
                pthread_mutex_lock(&ctx.metadata_lock);
            }
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
    }

    /* Shutdown and join logger thread */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_tid, NULL);

    /* Cleanup */
    close(ctx.server_fd);
    close(sfd);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    {
        container_record_t *c = ctx.containers, *next;
        while (c) {
            next = c->next;
            pthread_mutex_destroy(&c->run_mutex);
            pthread_cond_destroy(&c->run_cond);
            free(c);
            c = next;
        }
    }

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stderr, "[supervisor] shutdown complete — no zombies\n");
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * CLIENT-SIDE CONTROL REQUEST
 *
 * Connects to the supervisor's UNIX socket, sends the request struct,
 * then reads responses until status != 1 (not more-data).
 * For CMD_RUN this blocks until the supervisor sends the final status.
 * ═══════════════════════════════════════════════════════════════════ */

/* State shared between the CMD_RUN main thread and the stop-forwarder */
static volatile sig_atomic_t g_run_stop_flag = 0;
static char g_run_stop_id[CONTAINER_ID_LEN];

static void run_sighandler(int sig)
{
    (void)sig;
    g_run_stop_flag = 1;
}

static void *run_stop_watcher(void *arg)
{
    (void)arg;
    while (!g_run_stop_flag)
        usleep(100000);   /* poll at 100 ms */

    /* Forward stop to supervisor via a new connection */
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, g_run_stop_id, CONTAINER_ID_LEN - 1);
    send_control_request(&req);
    return NULL;
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send");
        close(fd);
        return 1;
    }

    while ((n = recv(fd, &resp, sizeof(resp), MSG_WAITALL)) ==
           (ssize_t)sizeof(resp)) {
        if (resp.message[0])
            printf("%s\n", resp.message);
        if (resp.status != 1)
            break;
    }

    close(fd);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * COMMAND DISPATCH  (client-side entry points)
 * ═══════════════════════════════════════════════════════════════════ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_CMD_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5))
        return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    pthread_t watcher;
    struct sigaction sa;
    int rc;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <rootfs> <command>"
                " [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_CMD_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5))
        return 1;

    /* Set up SIGINT/SIGTERM forwarding to the supervisor */
    strncpy(g_run_stop_id, req.container_id, CONTAINER_ID_LEN - 1);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = run_sighandler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    pthread_create(&watcher, NULL, run_stop_watcher, NULL);
    pthread_detach(watcher);

    rc = send_control_request(&req);
    g_run_stop_flag = 1;   /* signal watcher to exit if container already done */
    return rc;
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ═══════════════════════════════════════════════════════════════════
 * MAIN
 * ═══════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
