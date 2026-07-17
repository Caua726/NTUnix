#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void check(int condition, const char *name)
{
    printf("%s - %s\n", condition ? "ok" : "FAIL", name);
    if (!condition) ++failures;
}

int main(void)
{
    const char payload[] = "musl-nt file io\n";
    char buffer[64] = {0};
    struct stat st;
    struct statvfs fs;
    struct timespec ts;
    regex_t regex;
    DIR *directory;
    struct dirent *entry;
    int saw_file = 0;
    int fd;

    mkdir("smoke-dir", 0755);
    fd = open("smoke-dir/input.txt", O_CREAT | O_TRUNC | O_RDWR, 0644);
    check(fd >= 0, "openat/CreateFileW");
    check(fd >= 0 && write(fd, payload, sizeof payload - 1) == (ssize_t)(sizeof payload - 1),
          "write/WriteFile");
    check(fd >= 0 && lseek(fd, 0, SEEK_SET) == 0, "lseek/SetFilePointerEx");
    check(fd >= 0 && read(fd, buffer, sizeof payload - 1) == (ssize_t)(sizeof payload - 1),
          "read/ReadFile");
    check(!memcmp(buffer, payload, sizeof payload - 1), "string module");
    check(fd >= 0 && !fstat(fd, &st) && st.st_size == (off_t)(sizeof payload - 1),
          "statx conversion");
    check(!statvfs(".", &fs) && fs.f_bsize && fs.f_blocks &&
          fs.f_bavail <= fs.f_blocks, "statfs/GetDiskFreeSpaceExW");
    if (fd >= 0) close(fd);

    directory = opendir("smoke-dir");
    while (directory && (entry = readdir(directory)))
        if (!strcmp(entry->d_name, "input.txt")) saw_file = 1;
    check(saw_file, "getdents64/NtQueryDirectoryFile");
    if (directory) closedir(directory);

    check(!clock_gettime(CLOCK_MONOTONIC, &ts) && ts.tv_sec >= 0,
          "clock_gettime/QPC");
    check(!regcomp(&regex, "^musl-nt [a-z]+ io$", REG_EXTENDED) &&
          !regexec(&regex, "musl-nt file io", 0, 0, 0), "regex module");
    regfree(&regex);

    check(!rename("smoke-dir/input.txt", "smoke-dir/output.txt"),
          "rename/MoveFileExW");
    check(!unlink("smoke-dir/output.txt"), "unlink/DeleteFileW");
    check(!rmdir("smoke-dir"), "rmdir/RemoveDirectoryW");
    check(malloc(128) != 0, "malloc over mmap/brk");
    return failures != 0;
}
