/*
 * tkuinput - test helper. Creates a synthetic multitouch "touchscreen" via
 * /dev/uinput that looks to the kernel exactly like a real panel (same
 * ABS_MT_POSITION_X/Y capabilities, non-virtual bustype), then injects one
 * tap per second.
 *
 * Used to prove EVIOCGRAB semantics on a real device: a second reader
 * (getevent) sees the taps until touchkillerd grabs the node, sees nothing
 * while the grab is held, and sees them again the instant it is released.
 * This is the same code path a physical finger takes.
 *
 * Not part of the shipped module.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

static void emit(int fd, int type, int code, int val)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = val;
    if (write(fd, &ev, sizeof(ev)) < 0)
        perror("write");
}

int main(int argc, char **argv)
{
    struct uinput_setup us;
    struct uinput_abs_setup abs;
    int fd, secs = argc > 1 ? atoi(argv[1]) : 60;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open /dev/uinput"); return 1; }

    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    memset(&abs, 0, sizeof(abs));
    abs.code = ABS_MT_POSITION_X;
    abs.absinfo.minimum = 0; abs.absinfo.maximum = 1080;
    ioctl(fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_MT_POSITION_Y;
    abs.absinfo.maximum = 2400;
    ioctl(fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_MT_SLOT;
    abs.absinfo.maximum = 9;
    ioctl(fd, UI_ABS_SETUP, &abs);
    abs.code = ABS_MT_TRACKING_ID;
    abs.absinfo.maximum = 65535;
    ioctl(fd, UI_ABS_SETUP, &abs);

    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_I2C;      /* not BUS_VIRTUAL: must pass the filter */
    us.id.vendor = 0x7443;
    us.id.product = 0x4b31;
    snprintf(us.name, sizeof(us.name), "tk_probe_panel");

    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) { perror("UI_DEV_SETUP"); return 1; }
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); return 1; }

    fprintf(stderr, "tk_probe_panel created; injecting 1 tap/s for %ds\n", secs);
    fflush(stderr);

    for (int i = 0; i < secs; i++) {
        emit(fd, EV_ABS, ABS_MT_SLOT, 0);
        emit(fd, EV_ABS, ABS_MT_TRACKING_ID, i + 1);
        emit(fd, EV_ABS, ABS_MT_POSITION_X, 500);
        emit(fd, EV_ABS, ABS_MT_POSITION_Y, 900);
        emit(fd, EV_KEY, BTN_TOUCH, 1);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        usleep(50000);
        emit(fd, EV_ABS, ABS_MT_SLOT, 0);
        emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
        emit(fd, EV_KEY, BTN_TOUCH, 0);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        printf("tap %d\n", i + 1);
        fflush(stdout);
        usleep(950000);
    }

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
