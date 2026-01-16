#include <linux/input-event-codes.h>
#include <stdio.h>

#include <linux/version.h>
#include <linux/input.h>

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>

#include <libudev.h>
#include <poll.h>


#define CHECK_READ(res, buf)                                                                                   \
if (res == -1) {                                                                                               \
    perror("read failed.");                                                                                    \
    goto exit;                                                                                                 \
}                                                                                                              \
if (res != sizeof(buf)) {                                                                                      \
    fprintf(stderr, "expected to read %d bytes, but readed only %d\n", (int) sizeof(struct input_event), res); \
    goto exit;                                                                                                 \
}                                                                                                              \

#define CHECK_WRITE(res, buf)                                                                                    \
if (res == -1) {                                                                                                 \
    perror("write failed.");                                                                                     \
    goto exit;                                                                                                   \
}                                                                                                                \
if (res != sizeof(buf)) {                                                                                        \
    fprintf(stderr, "expected to write %d bytes, but written only %d\n", (int) sizeof(struct input_event), res); \
    goto exit;                                                                                                   \
}                                                                                                                \

static volatile sig_atomic_t stop = 0;

static void interrupt_handler(int sig)
{
    stop = 1;
}

static int constructKeyboard (const char *name, struct input_id *id)
{
    struct uinput_user_dev description;
    int res = 0;
    int fd = 0;

    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("open /dev/uinput");
        goto err;
    }

    res = ioctl(fd, UI_SET_EVBIT, EV_KEY);
    if (res == -1) {
        perror("ioctl UI_SET_EVBIT EV_KEY");
        goto err;
    }

    for(int c = 0; c < KEY_MAX; c++) {
        res = ioctl(fd, UI_SET_KEYBIT, c);
        if (res == -1) {
            fprintf(stderr, "ioctl UI_SET_KEYBIT %d\n", c);
            goto err;
        }
    }

    memset(&description, 0, sizeof(description));
    strncpy(description.name, name, UINPUT_MAX_NAME_SIZE);
    memcpy(&description.id, id, sizeof(description.id));

    res = ioctl(fd, UI_DEV_SETUP, &description);
    if (res == -1) {
        perror("ioctl UI_DEV_SETUP");
        goto err;
    }

    res = ioctl(fd, UI_DEV_CREATE);
    if (res == -1) {
        perror("ioctl UI_DEV_CREATE");
        goto err;
    }

    return fd;

err:
    if (fd != -1) {
        close(fd);
    }

    return -1;
}

static void wait_for_usb_connection()
{
    struct udev *udev;
    struct udev_monitor *mon;
    int fd;
    bool exit = false;

    udev = udev_new();
    if (!udev) {
        fprintf(stderr, "Can't create udev\n");
        sleep(1);
        return;
    }

    mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon) {
        fprintf(stderr, "Can't create udev monitor\n");
        udev_unref(udev);
        sleep(1);
        return;
    }

    udev_monitor_filter_add_match_subsystem_devtype(mon, "usb", NULL);

    udev_monitor_enable_receiving(mon);

    fd = udev_monitor_get_fd(mon);

    printf("Monitoring USB events. Plug or unplug a USB device...\n");

    while (!exit) {
        struct pollfd fds[1];
        fds[0].fd = fd;
        fds[0].events = POLLIN;

        int ret = poll(fds, 1, -1);

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            struct udev_device *dev = udev_monitor_receive_device(mon);
            if (dev) {
                const char* action = udev_device_get_action(dev);
                const char* devpath = udev_device_get_devpath(dev);
                const char* vendor_id = udev_device_get_sysattr_value(dev, "idVendor");
                const char* model_id = udev_device_get_sysattr_value(dev, "idProduct");

                if (action && devpath) {
                    printf("Action: %s\n", action);
                    printf("Devpath: %s\n", devpath);
                    if (vendor_id && model_id) {
                        printf("VendorID: %s, ProductID: %s\n", vendor_id, model_id);
                        exit = true;
                    }
                }

                udev_device_unref(dev);
            }
        }
    }

    udev_monitor_unref(mon);
    udev_unref(udev);
}

int main(int argc, char *argv[])
{
    int fd = 0;
    int uinput = 0;
    int rc = 0;

    do {
        if ((fd = open(argv[1], O_RDONLY)) < 0) {
            perror("open failed.");
            if (errno == EACCES && getuid() != 0)
                fprintf(stderr, "You do not have access to %s. Try running as root instead.\n",	argv[1]);
            goto exit;
        }

        if (signal(SIGINT, interrupt_handler) == SIG_ERR) {
            perror("signal call for SIGINT failed.");
            goto exit;
        }

        if (signal(SIGTERM, interrupt_handler) == SIG_ERR) {
            perror("signal call for SIGTERM failed.");
            goto exit;
        }

        fd_set rdfs;

        FD_ZERO(&rdfs);
        FD_SET(fd, &rdfs);

        struct input_id id;
        if (ioctl(fd, EVIOCGID, &id) == -1) {
            perror("ioctl for EVIOCGID failed.");
            goto exit;
        }

        if ((id.bustype != 3)||(id.vendor != 1133)||(id.product != 50504)) {
            fprintf(stderr, "Wrong keyboard detected!\n");
            goto exit;
        }

        if (id.version != 273) {
            printf("WARNING!!! keyboard version 273 expected, but %d detected!\n", id.version);
        }

        uinput = constructKeyboard("Example device", &id);
        if (uinput == -1) {
            fprintf(stderr, "constructKeyboard call failed!\n");
            goto exit;
        }

        if (getppid() != 1) {
            printf("WARNING!!! Not started as daemon. Will delay 1sec!\n");
            sleep(1);
            printf("Continuing...\n");
        }

        if (ioctl(fd, EVIOCGRAB, (void*)1) == -1) {
            perror("ioctl for EVIOCGRAB 1 failed.");
            goto exit;
        }

        bool keep_converting = false;
        while(!stop)
        {
            struct input_event ev;
            int wr = 0;
            int rv = 0;
            int rd = 0;

            rd = select(fd + 1, &rdfs, NULL, NULL, NULL);

            if (stop) {
                break;
            }

            if (rd == -1) {
                perror("select failed.");
                goto exit;
            }

            rd = read(fd, &ev, sizeof(ev));
            CHECK_READ(rd, ev);


            if ((ev.code == KEY_LEFTMETA)&&(ev.type == 1)&&(ev.value == 1)) {
                struct timeval timeout;

                timeout.tv_sec = 0;
                timeout.tv_usec = 50000;
                rv = select(fd + 1, &rdfs, NULL, NULL, &timeout);
                if (rv == -1) {
                    perror("select failed.");
                    goto exit;
                }

                if (rv > 0) {
                    rd = read(fd, &ev, sizeof(ev));
                    CHECK_READ(rd, ev);

                    rd = read(fd, &ev, sizeof(ev));
                    CHECK_READ(rd, ev);

                    rd = read(fd, &ev, sizeof(ev));
                    CHECK_READ(rd, ev);

                    if ((ev.code == KEY_SPACE)&&(ev.type == 1)&&(ev.value == 1)) {
                        ev.code = KEY_INSERT; ev.type = 1; ev.value = 1;
                        wr = write(uinput, &ev, sizeof(ev));
                        CHECK_WRITE(wr, ev);

                        ev.code = 0; ev.type = 0; ev.value = 0;
                        wr = write(uinput, &ev, sizeof(ev));
                        CHECK_WRITE(wr, ev);

                        ev.code = KEY_INSERT; ev.type = 1; ev.value = 0;
                        wr = write(uinput, &ev, sizeof(ev));
                        CHECK_WRITE(wr, ev);

                        ev.code = 0; ev.type = 0; ev.value = 0;
                        wr = write(uinput, &ev, sizeof(ev));
                        CHECK_WRITE(wr, ev);

                        rd = read(fd, &ev, sizeof(ev));
                        CHECK_READ(rd, ev);

                        rd = read(fd, &ev, sizeof(ev));
                        CHECK_READ(rd, ev);

                        rd = read(fd, &ev, sizeof(ev));
                        CHECK_READ(rd, ev);

                        rd = read(fd, &ev, sizeof(ev));
                        CHECK_READ(rd, ev);

                        keep_converting = true;
                    }
                }
            }

            if ((keep_converting)&&(ev.code == KEY_LEFTMETA)&&(ev.type == 1)&&(ev.value == 0)) {
                keep_converting = false;
            }

            if ((keep_converting)&&(ev.code == KEY_SPACE)) {
                ev.code = KEY_INSERT;
            }

            wr = write(uinput, &ev, sizeof(ev));
            CHECK_WRITE(wr, ev);
        }

        rc = ioctl(fd, EVIOCGRAB, (void*)0);
        if (rc == -1) {
            perror("ioctl for EVIOCGRAB 0 failed.");
            goto exit;
        }

exit:
        if (uinput > 0) {
            close(uinput);
            uinput = 0;
        }
        if (fd > 0) {
            close(fd);
            fd = 0;
        }

        if (!stop) {
            wait_for_usb_connection();
        }

    } while(!stop);

    return 0;
}
