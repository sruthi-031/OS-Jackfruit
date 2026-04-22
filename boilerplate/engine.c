#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "monitor_ioctl.h"   


#define SOCKET_PATH "/tmp/mini_runtime.sock"
#define STACK_SIZE (1024*1024)
#define MAX 20

struct container {
    char name[32];
    pid_t pid;
    char state[16];
};

struct container table[MAX];
int count = 0;

/* ================= CHILD ================= */
int child_fn(void *arg)
{
    char **args = (char **)arg;

    char *rootfs = args[0];
    char *cmd    = args[1];
    char *name   = args[2];

    // 1. Enter container filesystem
    if (chroot(rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    // 2. Mount /proc
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount");
    }

    // 3. Create log file in HOST (important)
    char logfile[100];
    snprintf(logfile, sizeof(logfile), "%s.log", name);

    int fd = open(logfile, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        perror("open log");
    } else {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }

    // 4. Execute command
    char *exec_args[] = {cmd, NULL};
    execvp(cmd, exec_args);

    // If exec fails
    perror("execvp");
    return 1;
}
 


/* ================= RUN ================= */
void run_container(char *name, char *rootfs, char *cmd, int wait_flag)
{
    char *stack = malloc(STACK_SIZE);
    char *top = stack + STACK_SIZE;

    char *args[] = {rootfs, cmd, NULL};

    pid_t pid = clone(child_fn, top,
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
        args);

    if (pid < 0) {
        perror("clone");
        return;
    }

    printf("Container %s started PID=%d\n", name, pid);

    /* store */
    table[count].pid = pid;
    strcpy(table[count].name, name);
    strcpy(table[count].state, "running");
    count++;

    /* register with kernel monitor */
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd >= 0) {
        struct monitor_request req;
        req.pid = pid;
        req.soft_limit_bytes = 50 * 1024 * 1024;
        req.hard_limit_bytes = 100 * 1024 * 1024;
        strncpy(req.container_id, name, MONITOR_NAME_LEN);

        ioctl(fd, MONITOR_REGISTER, &req);
        close(fd);
    }

    if (wait_flag)
        waitpid(pid, NULL, 0);
}

/* ================= PS ================= */
void list_containers()
{
    int found = 0;

    for (int i = 0; i < count; i++) {
        if (kill(table[i].pid, 0) == 0) {
            if (!found) {
                printf("Running containers:\n");
                found = 1;
            }
            printf(" - %s (PID=%d)\n", table[i].name, table[i].pid);
        }
    }

    if (!found)
        printf("No running containers\n");
}

/* ================= STOP ================= */
void stop_container(char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(table[i].name, name) == 0) {
            kill(table[i].pid, SIGKILL);
            printf("Stopped %s\n", name);
            return;
        }
    }
    printf("Container not found\n");
}

/* ================= LOGS ================= */
void show_logs(char *name)
{
    char file[100];
    sprintf(file, "logs/%s.log", name);

    FILE *fp = fopen(file, "r");
    if (!fp) {
        printf("No logs\n");
        return;
    }

    char c;
    while ((c = fgetc(fp)) != EOF)
        putchar(c);

    fclose(fp);
}

/* ================= SUPERVISOR ================= */
void start_supervisor()
{
    signal(SIGCHLD, SIG_IGN);

    int server_fd, client_fd;
    struct sockaddr_un addr;

    unlink(SOCKET_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor running...\n");

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);

        char buffer[256] = {0};
        read(client_fd, buffer, sizeof(buffer));

        char cmd[50]={0}, name[50]={0}, rootfs[100]={0}, exec_cmd[100]={0};
        sscanf(buffer, "%s %s %s %s", cmd, name, rootfs, exec_cmd);

        if (strcmp(cmd, "start") == 0) {
            run_container(name, rootfs, exec_cmd, 0);
        }
        else if (strcmp(cmd, "run") == 0) {
            run_container(name, rootfs, exec_cmd, 1);
        }
        else if (strcmp(cmd, "ps") == 0) {
            list_containers();
        }
        else if (strcmp(cmd, "stop") == 0) {
            stop_container(name);
        }
        else if (strcmp(cmd, "logs") == 0) {
            show_logs(name);
        }
        else {
            printf("Invalid command\n");
        }

        close(client_fd);
    }
}

/* ================= CLIENT ================= */
void send_command(int argc, char *argv[])
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    char buffer[256];

    if (argc == 2)
        sprintf(buffer, "%s", argv[1]);
    else if (argc == 3)
        sprintf(buffer, "%s %s", argv[1], argv[2]);
    else
        sprintf(buffer, "%s %s %s %s", argv[1], argv[2], argv[3], argv[4]);

    write(sock, buffer, strlen(buffer));
    close(sock);
}

/* ================= MAIN ================= */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage:\n");
        printf(" ./engine supervisor\n");
        printf(" ./engine start <name> <rootfs> <cmd>\n");
        printf(" ./engine run <name> <rootfs> <cmd>\n");
        printf(" ./engine ps\n");
        printf(" ./engine logs <name>\n");
        printf(" ./engine stop <name>\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0)
        start_supervisor();
    else
        send_command(argc, argv);

    return 0;
}
