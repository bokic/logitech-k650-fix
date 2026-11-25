#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define LOGITECH_VENDOR_ID       0x046d /* 1133 */
#define LOGITECH_K650_PRODUCT_ID 0xc548 /* 50504 */
#define LOGITECH_K650_VERSION    0x0111 /* 273 */

#define CHECK_READ(res, buf)                                                                                      \
    do {                                                                                                          \
        if ((res) == -1) {                                                                                        \
            perror("read failed.");                                                                               \
            goto exit;                                                                                            \
        }                                                                                                         \
        if ((size_t)(res) != sizeof(buf)) {                                                                       \
            fprintf(stderr, "expected to read %zu bytes, but read only %zd\n", sizeof(struct input_event), (res)); \
            goto exit;                                                                                            \
        }                                                                                                         \
    } while (0)

static int write_event(int fd, const struct input_event *ev)
{
    for (;;) {
        ssize_t wr = write(fd, ev, sizeof(*ev));
        if (wr == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                if (poll(&pfd, 1, -1) == -1 && errno != EINTR) {
                    perror("poll uinput failed.");
                    return -1;
                }
                continue;
            }
            perror("write failed.");
            return -1;
        }
        if ((size_t)wr != sizeof(*ev)) {
            fprintf(stderr, "expected to write %zu bytes, but wrote only %zd\n", sizeof(*ev), wr);
            return -1;
        }
        return 0;
    }
}

static int flush_events(int fd, const struct input_event *evs, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (write_event(fd, &evs[i]) == -1) {
            return -1;
        }
    }
    return 0;
}

static volatile sig_atomic_t stop = 0;

static void interrupt_handler(int sig)
{
    (void)sig;
    stop = 1;
}

static int constructKeyboard (const char *name, struct input_id *id)
{
    struct uinput_setup description;
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

    res = ioctl(fd, UI_SET_EVBIT, EV_MSC);
    if (res == -1) {
        perror("ioctl UI_SET_EVBIT EV_MSC");
        goto err;
    }

    res = ioctl(fd, UI_SET_MSCBIT, MSC_SCAN);
    if (res == -1) {
        perror("ioctl UI_SET_MSCBIT MSC_SCAN");
        goto err;
    }

    res = ioctl(fd, UI_SET_EVBIT, EV_REP);
    if (res == -1) {
        perror("ioctl UI_SET_EVBIT EV_REP");
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
    strncpy(description.name, name, UINPUT_MAX_NAME_SIZE - 1);
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

static void wait_for_usb_connection(const char *dev_path)
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

    udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL);

    udev_monitor_enable_receiving(mon);

    if (access(dev_path, F_OK) == 0) {
        exit = true;
    }

    fd = udev_monitor_get_fd(mon);

    if (!exit) {
        printf("Waiting for input device %s...\n", dev_path);
    }

    while (!exit && !stop) {
        struct pollfd fds[1];
        fds[0].fd = fd;
        fds[0].events = POLLIN;

        int ret = poll(fds, 1, -1);
        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll failed");
            break;
        }

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            struct udev_device *dev = udev_monitor_receive_device(mon);
            if (dev) {
                const char *action = udev_device_get_action(dev);
                const char *devpath = udev_device_get_devpath(dev);

                if (action && devpath && strcmp(action, "add") == 0) {
                    if (access(dev_path, F_OK) == 0) {
                        printf("Device %s reconnected (%s).\n", dev_path, devpath);
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
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <event-device>\n", argc > 0 ? argv[0] : "logitech-k650-fix");
        return EXIT_FAILURE;
    }

    int fd = -1;
    int uinput = -1;
    int rc = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = interrupt_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction call for SIGINT failed.");
        return EXIT_FAILURE;
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction call for SIGTERM failed.");
        return EXIT_FAILURE;
    }

    do {
        if ((fd = open(argv[1], O_RDONLY)) < 0) {
            perror("open failed.");
            if (errno == EACCES && getuid() != 0)
                fprintf(stderr, "You do not have access to %s. Try running as root instead.\n",	argv[1]);
            goto exit;
        }

        struct input_id id;
        if (ioctl(fd, EVIOCGID, &id) == -1) {
            perror("ioctl for EVIOCGID failed.");
            goto exit;
        }

        if ((id.bustype != BUS_USB) || (id.vendor != LOGITECH_VENDOR_ID) || (id.product != LOGITECH_K650_PRODUCT_ID)) {
            fprintf(stderr, "Wrong keyboard detected!\n");
            goto exit;
        }

        if (id.version != LOGITECH_K650_VERSION) {
            printf("WARNING!!! keyboard version %d expected, but %d detected!\n", LOGITECH_K650_VERSION, id.version);
        }

        char dev_name[UINPUT_MAX_NAME_SIZE] = "Logitech K650";
        if (ioctl(fd, EVIOCGNAME(sizeof(dev_name)), dev_name) == -1) {
            perror("ioctl for EVIOCGNAME failed.");
        }

        uinput = constructKeyboard(dev_name, &id);
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
        bool has_pending_msc = false;
        bool suppress_next_syn = false;
        struct input_event pending_msc;
        while(!stop)
        {
            struct input_event ev;
            int rv = 0;
            ssize_t rd = 0;

            fd_set rdfs;
            FD_ZERO(&rdfs);
            FD_SET(fd, &rdfs);
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

            if ((ev.type == EV_MSC) && (ev.code == MSC_SCAN)) {
                if (has_pending_msc && write_event(uinput, &pending_msc) == -1) {
                    goto exit;
                }
                pending_msc = ev;
                has_pending_msc = true;
                continue;
            }

            if ((ev.type == EV_SYN) && (ev.code == SYN_REPORT)) {
                if (suppress_next_syn) {
                    suppress_next_syn = false;
                    continue;
                }
                if (has_pending_msc) {
                    if (write_event(uinput, &pending_msc) == -1) {
                        goto exit;
                    }
                    has_pending_msc = false;
                }
                if (write_event(uinput, &ev) == -1) {
                    goto exit;
                }
                continue;
            }

            if (!keep_converting && (ev.code == KEY_LEFTMETA) && (ev.type == EV_KEY) && (ev.value == 1)) {
                struct input_event pending[16];
                size_t pending_len = 0;
                bool matched_space = false;

                if (has_pending_msc) {
                    pending[pending_len++] = pending_msc;
                    has_pending_msc = false;
                }
                pending[pending_len++] = ev;

                while (pending_len < (sizeof(pending) / sizeof(pending[0]))) {
                    struct timeval timeout;
                    timeout.tv_sec = 0;
                    timeout.tv_usec = 50000;

                    FD_ZERO(&rdfs);
                    FD_SET(fd, &rdfs);
                    rv = select(fd + 1, &rdfs, NULL, NULL, &timeout);
                    if (stop) {
                        break;
                    }
                    if (rv == -1) {
                        perror("select failed.");
                        goto exit;
                    }
                    if (rv == 0) {
                        break;
                    }

                    rd = read(fd, &ev, sizeof(ev));
                    CHECK_READ(rd, ev);

                    if ((ev.type == EV_KEY) && (ev.code == KEY_SPACE) && (ev.value == 1)) {
                        matched_space = true;
                        break;
                    }

                    pending[pending_len++] = ev;

                    if (ev.type == EV_KEY) {
                        break;
                    }
                }

                if (stop) {
                    break;
                }

                if (matched_space) {
                    ev.type = EV_KEY; ev.code = KEY_INSERT; ev.value = 1;
                    if (write_event(uinput, &ev) == -1) {
                        goto exit;
                    }

                    ev.type = EV_SYN; ev.code = SYN_REPORT; ev.value = 0;
                    if (write_event(uinput, &ev) == -1) {
                        goto exit;
                    }

                    keep_converting = true;
                    suppress_next_syn = true;
                    continue;
                } else {
                    suppress_next_syn = false;
                    if (flush_events(uinput, pending, pending_len) == -1) {
                        goto exit;
                    }
                    continue;
                }
            }

            if (keep_converting && (ev.type == EV_KEY) && (ev.code == KEY_LEFTMETA)) {
                has_pending_msc = false;
                suppress_next_syn = true;
                if (ev.value == 0) {
                    keep_converting = false;
                }
                continue;
            }

            if (keep_converting && (ev.type == EV_KEY) && (ev.code == KEY_SPACE)) {
                ev.code = KEY_INSERT;
            }

            suppress_next_syn = false;
            if (has_pending_msc) {
                if (write_event(uinput, &pending_msc) == -1) {
                    goto exit;
                }
                has_pending_msc = false;
            }

            if (write_event(uinput, &ev) == -1) {
                goto exit;
            }
        }

        rc = ioctl(fd, EVIOCGRAB, (void*)0);
        if (rc == -1) {
            perror("ioctl for EVIOCGRAB 0 failed.");
            goto exit;
        }

exit:
        if (uinput >= 0) {
            if (ioctl(uinput, UI_DEV_DESTROY) == -1) {
                perror("ioctl UI_DEV_DESTROY");
            }
            close(uinput);
            uinput = -1;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }

        if (!stop) {
            wait_for_usb_connection(argv[1]);
        }

    } while(!stop);

    return EXIT_SUCCESS;
}
