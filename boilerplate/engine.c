#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)

typedef struct {
    char id[32];
    char rootfs[256];
    char command[256];
    int nice_value;
    unsigned long soft_limit;
    unsigned long hard_limit;
} config_t;

/* ================= CHILD FUNCTION ================= */
static int child_fn(void *arg)
{
    config_t *cfg = (config_t *)arg;

    /* Set hostname */
    if (sethostname(cfg->id, strlen(cfg->id)) != 0) {
        perror("sethostname failed");
    }

    /* Change root filesystem */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot failed");
        return 1;
    }

    if (chdir("/") != 0) {
        perror("chdir failed");
        return 1;
    }

    /* Mount /proc */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount failed");
        return 1;
    }

    /* Set nice value */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) == -1) {
            perror("nice failed");
        }
    }

    /* Print container details */
    char hostname[64];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        perror("gethostname failed");
        strcpy(hostname, "unknown");
    }

    printf("\n===== CONTAINER START =====\n");
    printf("Container ID : %s\n", cfg->id);
    printf("PID          : %d\n", getpid());
    printf("Hostname     : %s\n", hostname);
    printf("Nice Value   : %d\n", cfg->nice_value);
    printf("===========================\n\n");
    fflush(stdout);

    /* Execute command */
    execlp(cfg->command, cfg->command, NULL);

    perror("exec failed");
    return 1;
}

/* ================= REGISTER MONITOR ================= */
void register_monitor(pid_t pid, config_t *cfg)
{
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) {
        perror("open monitor failed");
        return;
    }

    struct monitor_request req;
    memset(&req, 0, sizeof(req));

    req.pid = pid;
    req.soft_limit_bytes = cfg->soft_limit;
    req.hard_limit_bytes = cfg->hard_limit;

    strncpy(req.container_id, cfg->id, MONITOR_NAME_LEN - 1);

    if (ioctl(fd, MONITOR_REGISTER, &req) < 0) {
        perror("ioctl register failed");
    }

    close(fd);
}

/* ================= USAGE ================= */
void usage(char *prog)
{
    printf("Usage:\n");
    printf("%s run <id> <rootfs> <cmd> [--nice N] [--soft-mib X] [--hard-mib Y]\n", prog);
}

/* ================= MAIN ================= */
int main(int argc, char *argv[])
{
    if (argc < 5) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "run") != 0) {
        usage(argv[0]);
        return 1;
    }

    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.id, argv[2], sizeof(cfg.id) - 1);
    strncpy(cfg.rootfs, argv[3], sizeof(cfg.rootfs) - 1);
    strncpy(cfg.command, argv[4], sizeof(cfg.command) - 1);

    /* Defaults */
    cfg.nice_value = 0;
    cfg.soft_limit = 50 * 1024 * 1024;   // 50MB
    cfg.hard_limit = 100 * 1024 * 1024;  // 100MB

    /* Parse optional arguments */
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--nice") == 0 && i + 1 < argc) {
            cfg.nice_value = atoi(argv[++i]);
        } 
        else if (strcmp(argv[i], "--soft-mib") == 0 && i + 1 < argc) {
            cfg.soft_limit = atol(argv[++i]) * 1024 * 1024;
        } 
        else if (strcmp(argv[i], "--hard-mib") == 0 && i + 1 < argc) {
            cfg.hard_limit = atol(argv[++i]) * 1024 * 1024;
        }
    }

    /* Allocate stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        perror("malloc failed");
        exit(1);
    }

    /* Create container */
    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | SIGCHLD,
                      &cfg);

    if (pid < 0) {
        perror("clone failed");
        exit(1);
    }

    printf("Started container '%s' with PID %d\n", cfg.id, pid);

    /* Register with kernel monitor */
    register_monitor(pid, &cfg);

    /* Wait for container */
    if (waitpid(pid, NULL, 0) < 0) {
        perror("waitpid failed");
    }

    printf("Container '%s' exited\n", cfg.id);

    free(stack);
    return 0;
}
