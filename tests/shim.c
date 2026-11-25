/*
 * LD_PRELOAD shim that lets the real logitech-k650-fix binary run without
 * hardware or root privileges.
 *
 * - open("K650_FAKE_INPUT") returns a dup of the pipe fd in $K650_IN_FD.
 * - open("/dev/uinput") returns a regular file at $K650_OUT.
 * - ioctl() on fake_input_fd emulates a K650 Bolt keyboard (EVIOCGID,
 *   EVIOCGNAME, EVIOCGPHYS, EVIOCGBIT, EVIOCGKEY, EVIOCGRAB), with optional
 *   fault injection via environment variables:
 *     * K650_FAKE_VID / K650_FAKE_PID: override USB vendor/product IDs
 *     * K650_FAKE_NO_PHYS=1: fail EVIOCGPHYS with ENOENT (emulates uinput)
 *     * K650_FAKE_MOUSE_ONLY=1: advertise BTN_LEFT without keyboard keys
 *     * K650_FAKE_EBUSY=1: fail EVIOCGRAB with EBUSY
 * - ioctl() on fake_uinput_fd records enabled UI_SET_KEYBIT codes to
 *   $K650_CAPS_FILE and creates $K650_READY_FILE on UI_DEV_CREATE.
 * - close() clears tracked fake fds so recycled fd numbers are never hijacked.
 * - On EOF on fake_input_fd, SIGTERM is raised and read() returns -1/EINTR so
 *   that signalfd + poll() shuts down cleanly with exit status 0.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#define FAKE_INPUT_PATH "K650_FAKE_INPUT"
#define KEY_BITMAP_BYTES ((KEY_CNT + 7) / 8)

static int fake_input_fd = -1;
static int fake_uinput_fd = -1;
static uint8_t uinput_keybits[KEY_BITMAP_BYTES];

typedef union {
    void *ptr;
    int (*open_fn)(const char *, int, ...);
    int (*close_fn)(int);
    ssize_t (*read_fn)(int, void *, size_t);
#ifdef __GLIBC__
    int (*ioctl_fn)(int, unsigned long, ...);
#else
    int (*ioctl_fn)(int, int, ...);
#endif
} sym_cast_t;

static int real_open_call(const char *path, int flags, mode_t mode)
{
    static int (*real_open)(const char *, int, ...);
    if (!real_open) {
        sym_cast_t u = { .ptr = dlsym(RTLD_NEXT, "open") };
        real_open = u.open_fn;
    }
    return real_open(path, flags, mode);
}

static int real_close_call(int fd)
{
    static int (*real_close)(int);
    if (!real_close) {
        sym_cast_t u = { .ptr = dlsym(RTLD_NEXT, "close") };
        real_close = u.close_fn;
    }
    return real_close(fd);
}

static int shim_open(const char *path, int flags, mode_t mode)
{
    if (strcmp(path, "/dev/uinput") == 0) {
        const char *out = getenv("K650_OUT");
        if (!out) {
            errno = EACCES;
            return -1;
        }
        memset(uinput_keybits, 0, sizeof(uinput_keybits));
        fake_uinput_fd = real_open_call(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        return fake_uinput_fd;
    }
    if (strcmp(path, FAKE_INPUT_PATH) == 0) {
        const char *in = getenv("K650_IN_FD");
        if (!in) {
            errno = ENOENT;
            return -1;
        }
        fake_input_fd = dup(atoi(in));
        return fake_input_fd;
    }
    return real_open_call(path, flags, mode);
}

static mode_t extract_mode(int flags, va_list ap)
{
    if (flags & (O_CREAT | O_TMPFILE)) {
        return (mode_t)va_arg(ap, int);
    }
    return 0;
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    mode_t mode;
    va_start(ap, flags);
    mode = extract_mode(flags, ap);
    va_end(ap);
    return shim_open(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    va_list ap;
    mode_t mode;
    va_start(ap, flags);
    mode = extract_mode(flags, ap);
    va_end(ap);
    return shim_open(path, flags, mode);
}

int __open_2(const char *path, int flags);
int __open64_2(const char *path, int flags);
int __open_2(const char *path, int flags) { return shim_open(path, flags, 0); }
int __open64_2(const char *path, int flags) { return shim_open(path, flags, 0); }

int close(int fd)
{
    if (fd >= 0 && fd == fake_input_fd) {
        fake_input_fd = -1;
    }
    if (fd >= 0 && fd == fake_uinput_fd) {
        fake_uinput_fd = -1;
    }
    return real_close_call(fd);
}

static void set_bit_buf(uint8_t *buf, size_t len, unsigned int bit)
{
    size_t idx = bit / 8U;
    if (idx < len) {
        buf[idx] |= (uint8_t)(1U << (bit % 8U));
    }
}

static int write_str_ioctl(void *arg, size_t maxlen, const char *str)
{
    if (maxlen == 0) {
        return 0;
    }
    size_t slen = strlen(str) + 1U;
    size_t n = slen < maxlen ? slen : maxlen;
    memcpy(arg, str, n);
    ((char *)arg)[maxlen - 1U] = '\0';
    return (int)n;
}

#ifdef __GLIBC__
typedef unsigned long ioctl_req_t;
#else
typedef int ioctl_req_t;
#endif

int ioctl(int fd, ioctl_req_t request, ...)
{
    va_list ap;
    void *arg;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    unsigned long ureq = (unsigned long)request;

    if (fd >= 0 && fd == fake_input_fd) {
        if (ureq == (unsigned long)EVIOCGID) {
            unsigned int vid = 0x046dU;
            unsigned int pid = 0xc548U;
            const char *ev_vid = getenv("K650_FAKE_VID");
            const char *ev_pid = getenv("K650_FAKE_PID");
            if (ev_vid) {
                vid = (unsigned int)strtoul(ev_vid, NULL, 0);
            }
            if (ev_pid) {
                pid = (unsigned int)strtoul(ev_pid, NULL, 0);
            }
            struct input_id id = { BUS_USB, (uint16_t)vid, (uint16_t)pid, 0x0111 };
            memcpy(arg, &id, sizeof(id));
            return 0;
        }

        unsigned long nr_nosize = ureq & ~((unsigned long)_IOC_SIZEMASK << _IOC_SIZESHIFT);
        size_t len = _IOC_SIZE(ureq);

        if (nr_nosize == (unsigned long)EVIOCGNAME(0)) {
            return write_str_ioctl(arg, len, "Logitech USB Receiver");
        }
        if (nr_nosize == (unsigned long)EVIOCGPHYS(0)) {
            const char *no_phys = getenv("K650_FAKE_NO_PHYS");
            if (no_phys && strcmp(no_phys, "1") == 0) {
                errno = ENOENT;
                return -1;
            }
            return write_str_ioctl(arg, len, "usb-0000:00:14.0-1/input0");
        }
        if (nr_nosize == (unsigned long)EVIOCGKEY(0)) {
            if (len > 0) {
                memset(arg, 0, len);
            }
            return (int)len;
        }
        if (_IOC_DIR(ureq) == _IOC_READ && _IOC_TYPE(ureq) == 'E' &&
            _IOC_NR(ureq) >= 0x20 && _IOC_NR(ureq) < (0x20 + EV_CNT)) {
            unsigned int ev_type = (unsigned int)(_IOC_NR(ureq) - 0x20);
            uint8_t *bits = (uint8_t *)arg;
            if (len > 0) {
                memset(bits, 0, len);
            }
            if (ev_type == 0) {
                set_bit_buf(bits, len, EV_SYN);
                set_bit_buf(bits, len, EV_KEY);
                set_bit_buf(bits, len, EV_MSC);
                set_bit_buf(bits, len, EV_LED);
                set_bit_buf(bits, len, EV_REP);
            } else if (ev_type == EV_KEY) {
                const char *mouse_only = getenv("K650_FAKE_MOUSE_ONLY");
                if (mouse_only && strcmp(mouse_only, "1") == 0) {
                    set_bit_buf(bits, len, BTN_LEFT);
                    set_bit_buf(bits, len, BTN_RIGHT);
                } else {
                    /* Match real K650 keyboard interface (codes up to KEY_F24 = 0xc2). */
                    for (unsigned int c = 1; c <= 0xc2U; c++) {
                        if (c != KEY_POWER && c != KEY_SLEEP) {
                            set_bit_buf(bits, len, c);
                        }
                    }
                }
            } else if (ev_type == EV_MSC) {
                set_bit_buf(bits, len, MSC_SCAN);
            }
            return (int)len;
        }
        if (ureq == (unsigned long)EVIOCGRAB) {
            const char *ebusy = getenv("K650_FAKE_EBUSY");
            if (arg != NULL && ebusy && strcmp(ebusy, "1") == 0) {
                errno = EBUSY;
                return -1;
            }
            return 0;
        }
        return 0;
    }

    if (fd >= 0 && fd == fake_uinput_fd) {
        if (ureq == (unsigned long)UI_SET_KEYBIT) {
            unsigned int k = (unsigned int)(uintptr_t)arg;
            set_bit_buf(uinput_keybits, sizeof(uinput_keybits), k);
        } else if (ureq == (unsigned long)UI_DEV_CREATE) {
            const char *caps_path = getenv("K650_CAPS_FILE");
            if (caps_path) {
                int cfd = real_open_call(caps_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                if (cfd >= 0) {
                    ssize_t wr = write(cfd, uinput_keybits, sizeof(uinput_keybits));
                    (void)wr;
                    real_close_call(cfd);
                }
            }
            const char *ready_path = getenv("K650_READY_FILE");
            if (ready_path) {
                int rfd = real_open_call(ready_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                if (rfd >= 0) {
                    real_close_call(rfd);
                }
            }
        }
        return 0;
    }

    static int (*real_ioctl)(int, ioctl_req_t, ...);
    if (!real_ioctl) {
        sym_cast_t u = { .ptr = dlsym(RTLD_NEXT, "ioctl") };
        real_ioctl = u.ioctl_fn;
    }
    return real_ioctl(fd, request, arg);
}

static ssize_t after_read(int fd, ssize_t r)
{
    if (r == 0 && fd >= 0 && fd == fake_input_fd) {
        raise(SIGTERM);
        errno = EINTR;
        return -1;
    }
    return r;
}

ssize_t read(int fd, void *buf, size_t count)
{
    static ssize_t (*real_read)(int, void *, size_t);
    if (!real_read) {
        sym_cast_t u = { .ptr = dlsym(RTLD_NEXT, "read") };
        real_read = u.read_fn;
    }
    return after_read(fd, real_read(fd, buf, count));
}

ssize_t __read_chk(int fd, void *buf, size_t count, size_t buflen);
ssize_t __read_chk(int fd, void *buf, size_t count, size_t buflen)
{
    (void)buflen;
    return read(fd, buf, count);
}
