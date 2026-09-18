/*
 * tkgrabtest - test helper. Tries EVIOCGRAB on a node and reports the result.
 *
 * EVIOCGRAB is exclusive: while touchkillerd holds the grab on the real
 * touchscreen node, this returns EBUSY. That is direct kernel-level proof
 * that the panel's events are no longer being delivered to any other reader
 * (including system_server's InputReader), without needing a human finger.
 *
 * Not part of the shipped module.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

int main(int argc, char **argv)
{
    int fd, rc;

    if (argc < 2) {
        fprintf(stderr, "usage: tkgrabtest /dev/input/eventN\n");
        return 2;
    }
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("OPEN_FAILED %s\n", strerror(errno));
        return 1;
    }
    rc = ioctl(fd, EVIOCGRAB, 1);
    if (rc < 0) {
        printf("GRAB_DENIED errno=%d (%s)%s\n", errno, strerror(errno),
               errno == EBUSY ? "  <- someone else holds the grab" : "");
        close(fd);
        return 1;
    }
    ioctl(fd, EVIOCGRAB, 0);
    close(fd);
    printf("GRAB_OK  <- node is free, nobody is grabbing it\n");
    return 0;
}
