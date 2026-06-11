#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>

#define COWFS_IOC_MAGIC 'C'

struct cowfs_version_info {
    unsigned long long timestamp;
    unsigned int       op_type;
    char               shadow_path[256];
};

struct cowfs_list_req {
    char path[512];
    unsigned int max_count;
    unsigned int found_count;
    struct cowfs_version_info versions[64];
};

struct cowfs_rollback_req {
    char               path[512];
    unsigned long long timestamp;
};

#define COWFS_IOC_LIST     _IOWR('C', 1, struct cowfs_list_req)
#define COWFS_IOC_ROLLBACK _IOW('C',  2, struct cowfs_rollback_req)

#define CTL_DEV "/dev/cowfs_ctl"

static const char *op_name(unsigned int op)
{
    switch (op) {
    case 1: return "WRITE";
    case 2: return "UNLINK";
    case 3: return "RENAME";
    case 4: return "SETATTR";
    default: return "UNKNOWN";
    }
}

static void cmd_list(const char *path)
{
    int fd;
    struct cowfs_list_req req = {0};
    unsigned int i;
    char timebuf[64];

    fd = open(CTL_DEV, O_RDWR);
    if (fd < 0) {
        perror("open " CTL_DEV);
        exit(1);
    }

    strncpy(req.path, path, sizeof(req.path) - 1);
    req.max_count = 64;

    if (ioctl(fd, COWFS_IOC_LIST, &req) < 0) {
        perror("ioctl LIST");
        close(fd);
        exit(1);
    }
    close(fd);

    if (req.found_count == 0) {
        printf("No versions found for: %s\n", path);
        return;
    }

    printf("Versions for: %s\n", path);
    printf("%-22s %-10s %s\n", "TIMESTAMP", "OPERATION", "SHADOW");
    printf("%-22s %-10s %s\n", "---------", "---------", "------");

    for (i = 0; i < req.found_count; i++) {
        time_t ts = (time_t)req.versions[i].timestamp;
        struct tm *tm_info = localtime(&ts);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);
        printf("%-22s %-10s %s\n",
               timebuf,
               op_name(req.versions[i].op_type),
               req.versions[i].shadow_path[0]
                   ? req.versions[i].shadow_path : "(metadata only)");
    }
}

static void cmd_rollback(const char *path, const char *ts_str)
{
    int fd;
    struct cowfs_rollback_req req = {0};

    fd = open(CTL_DEV, O_RDWR);
    if (fd < 0) {
        perror("open " CTL_DEV);
        exit(1);
    }

    strncpy(req.path, path, sizeof(req.path) - 1);
    req.timestamp = (ts_str && strcmp(ts_str, "latest") != 0)
                    ? strtoull(ts_str, NULL, 10)
                    : 0;

    if (ioctl(fd, COWFS_IOC_ROLLBACK, &req) < 0) {
        perror("ioctl ROLLBACK");
        close(fd);
        exit(1);
    }
    close(fd);
    printf("Rollback successful: %s\n", path);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s list <path>\n"
        "  %s rollback <path> [timestamp|latest]\n",
        prog, prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc < 3)
        usage(argv[0]);

    if (strcmp(argv[1], "list") == 0)
        cmd_list(argv[2]);
    else if (strcmp(argv[1], "rollback") == 0)
        cmd_rollback(argv[2], argc >= 4 ? argv[3] : NULL);
    else
        usage(argv[0]);

    return 0;
}
