#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#if defined(__x86_64__) || defined(__i386__)
#include <sys/io.h>
#endif
#include <unistd.h>

static void console(const char *message) {
    int fd = open("/dev/console", O_WRONLY | O_CLOEXEC);
    if (fd < 0) fd = STDERR_FILENO;
    size_t left = strlen(message);
    while (left != 0) {
        ssize_t written = write(fd, message, left);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        message += written;
        left -= (size_t)written;
    }
    if (fd != STDERR_FILENO) close(fd);
}

static int vmm_signal(const char *message) {
#if defined(__x86_64__) || defined(__i386__)
    if (ioperm(0xe9, 1, 1) < 0 || ioperm(0x64, 1, 1) < 0) return -1;
    while (*message != '\0') outb((unsigned char)*message++, 0xe9);
    outb(0xfe, 0x64);
    return 0;
#elif defined(__aarch64__)
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) return -1;
    volatile unsigned char *port = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, fd, 0x10000000);
    if (port == MAP_FAILED) { close(fd); return -1; }
    while (*message != '\0') *port = (unsigned char)*message++;
    (void)munmap((void *)port, 4096);
    close(fd);
    (void)reboot(LINUX_REBOOT_CMD_RESTART);
    return -1;
#else
    (void)message;
    return -1;
#endif
}

int main(void) {
    (void)mkdir("/dev", 0755);
    (void)mkdir("/proc", 0555);
    (void)mkdir("/tmp", 01777);
    (void)mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    (void)mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, "");
    (void)mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "size=4m");

    int console_fd = open("/dev/console", O_RDWR | O_CLOEXEC);
    if (console_fd >= 0) {
        (void)dup2(console_fd, STDIN_FILENO);
        (void)dup2(console_fd, STDOUT_FILENO);
        (void)dup2(console_fd, STDERR_FILENO);
        if (console_fd > STDERR_FILENO) close(console_fd);
    }

    pid_t child = fork();
    if (child == 0) {
        char *const argv[] = {"/agent-task", NULL};
        char *const envp[] = {"PATH=/", NULL};
        execve(argv[0], argv, envp);
        _exit(127);
    }
    int status = 0;
    while (child > 0 && waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (child < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        console("CELL_MVP_TASK_FAILED\n");
        (void)vmm_signal("CELL_MVP_TASK_FAILED\n");
        for (;;) pause();
    }

    console("CELL_MVP_OK\n");
    sync();
    if (vmm_signal("CELL_MVP_OK\n") == 0)
        for (;;) pause();
    /* Restart is observable without ACPI: x86 writes the reset controller. */
    if (reboot(LINUX_REBOOT_CMD_RESTART) < 0)
        console("CELL_MVP_REBOOT_FAILED\n");
    for (;;) pause();
}
