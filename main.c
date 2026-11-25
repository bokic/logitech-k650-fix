#define _POSIX_C_SOURCE 200809L

#include "remap.h"

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

#define LOGITECH_VENDOR_ID       0x046d /* 1133 */
#define LOGITECH_K650_PRODUCT_ID 0xc548 /* 50504 */
#define LOGITECH_K650_VERSION    0x0111 /* 273 */

#define BITS_PER_LONG (sizeof(unsigned long) * 8U)
#define NBITS(x)      ((((x) - 1U) / BITS_PER_LONG) + 1U)
#define TEST_BIT(bit, array) \
    (((array[(unsigned int)(bit) / BITS_PER_LONG] >> ((unsigned int)(bit) % BITS_PER_LONG)) & 1UL) != 0UL)

enum open_status {
    OPEN_OK = 0,
    OPEN_TRANSIENT,
    OPEN_FATAL,
};

struct device_caps {
    struct input_id id;
    char name[UINPUT_MAX_NAME_SIZE];
    unsigned long evbits[NBITS(EV_CNT)];
    unsigned long keybits[NBITS(KEY_CNT)];
    unsigned long mscbits[NBITS(MSC_CNT)];
};

static int64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static bool drain_signalfd(int sigfd)
{
    bool got_signal = false;
    for (;;) {
        struct signalfd_siginfo fdsi;
        ssize_t r = read(sigfd, &fdsi, sizeof(fdsi));
        if (r == (ssize_t)sizeof(fdsi)) {
            got_signal = true;
            continue;
        }
        break;
    }
    return got_signal;
}

static bool wait_ms_interruptible(int sigfd, int ms, bool *stop)
{
    struct pollfd pfd = { .fd = sigfd, .events = POLLIN };
    int ret = poll(&pfd, 1, ms);
    if (ret > 0 && (pfd.revents & POLLIN)) {
        if (drain_signalfd(sigfd)) {
            *stop = true;
        }
    }
    return !(*stop);
}

static int write_event(int uinput_fd, int sigfd, bool *stop, const struct input_event *ev)
{
    for (;;) {
        ssize_t wr = write(uinput_fd, ev, sizeof(*ev));
        if (wr == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfds[2] = {
                    { .fd = uinput_fd, .events = POLLOUT },
                    { .fd = sigfd, .events = POLLIN },
                };
                int pr = poll(pfds, 2, -1);
                if (pr == -1) {
                    if (errno == EINTR) {
                        continue;
                    }
                    perror("poll uinput failed");
                    return -1;
                }
                if (pfds[1].revents & POLLIN) {
                    if (drain_signalfd(sigfd)) {
                        *stop = true;
                    }
                }
                continue;
            }
            perror("write uinput failed");
            return -1;
        }
        if ((size_t)wr != sizeof(*ev)) {
            fprintf(stderr, "expected to write %zu bytes, but wrote only %zd\n", sizeof(*ev), wr);
            return -1;
        }
        return 0;
    }
}

static int flush_out(int uinput_fd, int sigfd, bool *stop, struct k650_out *out)
{
    for (size_t i = 0; i < out->count; i++) {
        if (write_event(uinput_fd, sigfd, stop, &out->events[i]) == -1) {
            out->count = 0;
            return -1;
        }
    }
    out->count = 0;
    return 0;
}

static int construct_keyboard(const struct device_caps *caps)
{
    struct uinput_setup description;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd == -1) {
        perror("open /dev/uinput");
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) == -1) {
        perror("ioctl UI_SET_EVBIT EV_KEY");
        goto err;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_REP) == -1) {
        perror("ioctl UI_SET_EVBIT EV_REP");
        goto err;
    }

    for (unsigned int c = 0; c <= KEY_MAX; c++) {
        if (c == KEY_INSERT || TEST_BIT(c, caps->keybits)) {
            if (ioctl(fd, UI_SET_KEYBIT, c) == -1) {
                fprintf(stderr, "ioctl UI_SET_KEYBIT %u: %s\n", c, strerror(errno));
                goto err;
            }
        }
    }

    if (TEST_BIT(EV_MSC, caps->evbits)) {
        if (ioctl(fd, UI_SET_EVBIT, EV_MSC) == -1) {
            perror("ioctl UI_SET_EVBIT EV_MSC");
            goto err;
        }
        for (unsigned int m = 0; m <= MSC_MAX; m++) {
            if (TEST_BIT(m, caps->mscbits)) {
                if (ioctl(fd, UI_SET_MSCBIT, m) == -1) {
                    fprintf(stderr, "ioctl UI_SET_MSCBIT %u: %s\n", m, strerror(errno));
                    goto err;
                }
            }
        }
    }

    memset(&description, 0, sizeof(description));
    snprintf(description.name, sizeof(description.name), "%s", caps->name);
    memcpy(&description.id, &caps->id, sizeof(description.id));

    if (ioctl(fd, UI_DEV_SETUP, &description) == -1) {
        perror("ioctl UI_DEV_SETUP");
        goto err;
    }

    if (ioctl(fd, UI_DEV_CREATE) == -1) {
        perror("ioctl UI_DEV_CREATE");
        goto err;
    }

    return fd;

err:
    close(fd);
    return -1;
}

static bool any_key_pressed(const uint8_t *keys, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (keys[i] != 0U) {
            return true;
        }
    }
    return false;
}

static enum open_status wait_until_keys_released(int dev_fd, int sigfd, bool *stop)
{
    int64_t deadline = monotonic_ns() + 2000LL * 1000000LL;
    for (;;) {
        uint8_t keys[K650_KEY_BYTES];
        memset(keys, 0, sizeof(keys));
        if (ioctl(dev_fd, EVIOCGKEY(sizeof(keys)), keys) == -1) {
            if (errno == ENODEV || errno == ENXIO) {
                return OPEN_TRANSIENT;
            }
            perror("ioctl EVIOCGKEY failed");
            return OPEN_FATAL;
        }
        if (!any_key_pressed(keys, sizeof(keys))) {
            return OPEN_OK;
        }
        if (monotonic_ns() >= deadline) {
            return OPEN_OK;
        }
        if (!wait_ms_interruptible(sigfd, 15, stop)) {
            return OPEN_OK;
        }
    }
}

static enum open_status open_and_validate_source(const char *dev_path, int sigfd, bool *stop, int *out_fd, struct device_caps *caps)
{
    *out_fd = -1;
    memset(caps, 0, sizeof(*caps));

    int fd = open(dev_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENODEV || errno == ENXIO) {
            return OPEN_TRANSIENT;
        }
        perror("open failed");
        if (errno == EACCES && getuid() != 0) {
            fprintf(stderr, "You do not have access to %s. Try running as root instead.\n", dev_path);
        }
        return OPEN_FATAL;
    }

    if (ioctl(fd, EVIOCGID, &caps->id) == -1) {
        int err = errno;
        perror("ioctl EVIOCGID failed");
        close(fd);
        return (err == ENODEV || err == ENXIO) ? OPEN_TRANSIENT : OPEN_FATAL;
    }

    if (caps->id.bustype != BUS_USB ||
        caps->id.vendor != LOGITECH_VENDOR_ID ||
        caps->id.product != LOGITECH_K650_PRODUCT_ID) {
        fprintf(stderr, "Wrong keyboard detected (bus=0x%04x vendor=0x%04x product=0x%04x)!\n",
                caps->id.bustype, caps->id.vendor, caps->id.product);
        close(fd);
        return OPEN_FATAL;
    }

    if (caps->id.version != LOGITECH_K650_VERSION) {
        printf("WARNING: keyboard version 0x%04x expected, but 0x%04x detected.\n",
               (unsigned int)LOGITECH_K650_VERSION, (unsigned int)caps->id.version);
    }

    /* Reject virtual uinput devices (whose EVIOCGPHYS is NULL/-ENOENT or empty). */
    char phys[256];
    memset(phys, 0, sizeof(phys));
    int phys_rc = ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);
    if (phys_rc == -1) {
        int err = errno;
        if (err == ENODEV || err == ENXIO) {
            close(fd);
            return OPEN_TRANSIENT;
        }
        fprintf(stderr, "Input device %s has no physical path (virtual device rejected).\n", dev_path);
        close(fd);
        return OPEN_FATAL;
    }
    phys[sizeof(phys) - 1] = '\0';
    if (phys[0] == '\0') {
        fprintf(stderr, "Input device %s has empty physical path (virtual device rejected).\n", dev_path);
        close(fd);
        return OPEN_FATAL;
    }

    if (ioctl(fd, EVIOCGBIT(0, sizeof(caps->evbits)), caps->evbits) == -1 ||
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(caps->keybits)), caps->keybits) == -1) {
        int err = errno;
        perror("ioctl EVIOCGBIT failed");
        close(fd);
        return (err == ENODEV || err == ENXIO) ? OPEN_TRANSIENT : OPEN_FATAL;
    }

    if (TEST_BIT(EV_MSC, caps->evbits)) {
        if (ioctl(fd, EVIOCGBIT(EV_MSC, sizeof(caps->mscbits)), caps->mscbits) == -1) {
            int err = errno;
            perror("ioctl EVIOCGBIT EV_MSC failed");
            close(fd);
            return (err == ENODEV || err == ENXIO) ? OPEN_TRANSIENT : OPEN_FATAL;
        }
    }

    if (!TEST_BIT(EV_KEY, caps->evbits) ||
        !TEST_BIT(KEY_LEFTMETA, caps->keybits) ||
        !TEST_BIT(KEY_SPACE, caps->keybits)) {
        fprintf(stderr, "Input device %s lacks required keyboard capabilities.\n", dev_path);
        close(fd);
        return OPEN_FATAL;
    }

    snprintf(caps->name, sizeof(caps->name), "%s", "Logitech K650");
    if (ioctl(fd, EVIOCGNAME(sizeof(caps->name)), caps->name) == -1) {
        if (errno == ENODEV || errno == ENXIO) {
            close(fd);
            return OPEN_TRANSIENT;
        }
        perror("ioctl EVIOCGNAME failed");
    }
    caps->name[sizeof(caps->name) - 1] = '\0';

    enum open_status wait_st = wait_until_keys_released(fd, sigfd, stop);
    if (wait_st != OPEN_OK || *stop) {
        close(fd);
        return wait_st;
    }

    if (ioctl(fd, EVIOCGRAB, 1UL) == -1) {
        int err = errno;
        perror("ioctl EVIOCGRAB 1 failed");
        close(fd);
        return (err == ENODEV || err == ENXIO) ? OPEN_TRANSIENT : OPEN_FATAL;
    }

    *out_fd = fd;
    return OPEN_OK;
}

static void wait_for_usb_connection(const char *dev_path, int sigfd, bool *stop, bool after_disconnect)
{
    /* Avoid tight spin if a disconnect happened just before udev removes a stale symlink. */
    if (after_disconnect && access(dev_path, F_OK) == 0) {
        if (!wait_ms_interruptible(sigfd, 300, stop)) {
            return;
        }
    }

    struct udev *udev = udev_new();
    if (!udev) {
        fprintf(stderr, "Can't create udev context\n");
        wait_ms_interruptible(sigfd, 1000, stop);
        return;
    }

    struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon) {
        fprintf(stderr, "Can't create udev monitor\n");
        udev_unref(udev);
        wait_ms_interruptible(sigfd, 1000, stop);
        return;
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL) < 0 ||
        udev_monitor_enable_receiving(mon) < 0) {
        fprintf(stderr, "Failed to configure udev monitor\n");
        udev_monitor_unref(mon);
        udev_unref(udev);
        wait_ms_interruptible(sigfd, 1000, stop);
        return;
    }

    int mon_fd = udev_monitor_get_fd(mon);
    if (mon_fd < 0) {
        fprintf(stderr, "Invalid udev monitor fd\n");
        udev_monitor_unref(mon);
        udev_unref(udev);
        wait_ms_interruptible(sigfd, 1000, stop);
        return;
    }

    if (access(dev_path, F_OK) == 0) {
        udev_monitor_unref(mon);
        udev_unref(udev);
        return;
    }

    printf("Waiting for input device %s...\n", dev_path);

    while (!(*stop)) {
        struct pollfd fds[2] = {
            { .fd = mon_fd, .events = POLLIN },
            { .fd = sigfd, .events = POLLIN },
        };

        int ret = poll(fds, 2, -1);
        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll udev failed");
            wait_ms_interruptible(sigfd, 500, stop);
            break;
        }

        if (fds[1].revents & POLLIN) {
            if (drain_signalfd(sigfd)) {
                *stop = true;
                break;
            }
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "udev monitor poll error (revents=0x%x)\n", (unsigned int)fds[0].revents);
            wait_ms_interruptible(sigfd, 500, stop);
            break;
        }

        if (fds[0].revents & POLLIN) {
            struct udev_device *dev = udev_monitor_receive_device(mon);
            if (dev) {
                const char *action = udev_device_get_action(dev);
                const char *devpath = udev_device_get_devpath(dev);

                if (action && devpath && strcmp(action, "add") == 0 && access(dev_path, F_OK) == 0) {
                    printf("Device %s connected (%s).\n", dev_path, devpath);
                    udev_device_unref(dev);
                    break;
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
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <event-device>\n", argc > 0 ? argv[0] : "logitech-k650-fix");
        return EXIT_FAILURE;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        perror("sigprocmask failed");
        return EXIT_FAILURE;
    }

    int sigfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigfd == -1) {
        perror("signalfd failed");
        return EXIT_FAILURE;
    }

    int exit_code = EXIT_SUCCESS;
    int fd = -1;
    int uinput = -1;
    bool stop = false;
    bool after_disconnect = false;

    struct k650_state state;
    struct k650_out out;
    k650_state_init(&state);
    out.count = 0;

    while (!stop) {
        if (drain_signalfd(sigfd)) {
            stop = true;
            break;
        }

        struct device_caps caps;
        enum open_status st = open_and_validate_source(argv[1], sigfd, &stop, &fd, &caps);
        if (stop) {
            break;
        }
        if (st == OPEN_FATAL) {
            exit_code = EXIT_FAILURE;
            break;
        }
        if (st == OPEN_TRANSIENT) {
            wait_for_usb_connection(argv[1], sigfd, &stop, after_disconnect);
            after_disconnect = false;
            continue;
        }

        if (uinput < 0) {
            uinput = construct_keyboard(&caps);
            if (uinput < 0) {
                fprintf(stderr, "construct_keyboard failed!\n");
                (void)ioctl(fd, EVIOCGRAB, 0UL);
                close(fd);
                fd = -1;
                exit_code = EXIT_FAILURE;
                break;
            }
        }

        k650_state_init(&state);
        out.count = 0;
        bool fatal_io = false;

        while (!stop) {
            int timeout_ms = k650_timeout_ms(&state, monotonic_ns());
            struct pollfd pfds[2] = {
                { .fd = fd, .events = POLLIN },
                { .fd = sigfd, .events = POLLIN },
            };

            int pr = poll(pfds, 2, timeout_ms);
            if (pr == -1) {
                if (errno == EINTR) {
                    continue;
                }
                perror("poll failed");
                fatal_io = true;
                break;
            }

            if (pfds[1].revents & POLLIN) {
                if (drain_signalfd(sigfd)) {
                    stop = true;
                    break;
                }
            }

            int64_t now_ns = monotonic_ns();
            if (pr == 0) {
                k650_expire_combo(&state, now_ns, &out);
                if (flush_out(uinput, sigfd, &stop, &out) == -1) {
                    fatal_io = true;
                    break;
                }
                continue;
            }

            if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                struct input_event buf[64];
                ssize_t rd = read(fd, buf, sizeof(buf));
                if (rd == -1) {
                    if (errno == EINTR) {
                        if (drain_signalfd(sigfd)) {
                            stop = true;
                        }
                        continue;
                    }
                    if (errno == ENODEV || errno == ENXIO) {
                        after_disconnect = true;
                        break;
                    }
                    perror("read failed");
                    fatal_io = true;
                    break;
                }
                if (rd == 0 || ((size_t)rd % sizeof(buf[0])) != 0U) {
                    fprintf(stderr, "unexpected read size %zd (expected multiple of %zu)\n", rd, sizeof(buf[0]));
                    fatal_io = true;
                    break;
                }

                now_ns = monotonic_ns();
                size_t count = (size_t)rd / sizeof(buf[0]);
                for (size_t i = 0; i < count; i++) {
                    if (buf[i].type == EV_SYN && buf[i].code == SYN_DROPPED) {
                        uint8_t phys_keys[K650_KEY_BYTES];
                        memset(phys_keys, 0, sizeof(phys_keys));
                        const uint8_t *pk_ptr = NULL;
                        if (ioctl(fd, EVIOCGKEY(sizeof(phys_keys)), phys_keys) != -1) {
                            pk_ptr = phys_keys;
                        }
                        k650_handle_syn_dropped(&state, pk_ptr, sizeof(phys_keys), &out);
                    } else {
                        k650_process_event(&state, &buf[i], now_ns, &out);
                    }
                }

                if (flush_out(uinput, sigfd, &stop, &out) == -1) {
                    fatal_io = true;
                    break;
                }
            }
        }

        /* Release any keys held on the persistent virtual keyboard before closing fd. */
        if (uinput >= 0 && !fatal_io) {
            k650_release_all(&state, &out);
            if (flush_out(uinput, sigfd, &stop, &out) == -1) {
                fatal_io = true;
            }
        }

        if (fd >= 0) {
            if (stop) {
                (void)ioctl(fd, EVIOCGRAB, 0UL);
            }
            close(fd);
            fd = -1;
        }

        if (fatal_io) {
            exit_code = EXIT_FAILURE;
            break;
        }

        if (!stop) {
            wait_for_usb_connection(argv[1], sigfd, &stop, after_disconnect);
            after_disconnect = false;
        }
    }

    if (uinput >= 0) {
        if (ioctl(uinput, UI_DEV_DESTROY) == -1) {
            perror("ioctl UI_DEV_DESTROY");
        }
        close(uinput);
    }

    close(sigfd);
    return exit_code;
}
