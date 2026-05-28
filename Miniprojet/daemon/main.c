#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ssd1306.h"

#define DEAMON 0

// GPIO paths
#define GPIO_EXPORT "/sys/class/gpio/export"
#define GPIO_UNEXPORT "/sys/class/gpio/unexport"
#define GPIO_LED_NUM "362"
#define GPIO_LED_PATH "/sys/class/gpio/gpio" GPIO_LED_NUM

// Button GPIO numbers
#define BTN_S1 "0"
#define BTN_S2 "2"
#define BTN_S3 "3"

// Sysfs paths for fan module
#define MODE_PATH "/sys/class/fan_module_class/fan_module_device/mode"
#define FREQ_PATH "/sys/class/fan_module_class/fan_module_device/freq"
#define TEMP_PATH "/sys/class/fan_module_class/fan_module_device/temp"

// IPC socket path
#define SOCKET_PATH "/tmp/fan.sock"

// Timer durations
#define LED_FLASH_MS 50
#define SCREEN_UPDATE_MS 500

// Poll fd indices
#define FD_BTN_S1 0
#define FD_BTN_S2 1
#define FD_BTN_S3 2
#define FD_SOCKET 3
#define FD_TIMER_LED 4
#define FD_TIMER_SCREEN 5

#define NUM_FDS 6
#define NUM_BUTTONS 3

// Global flag for signal handler to indicate program should end
static volatile int end_program = 0;

// The signal handler is used only for testing when running the program directly
#if !DEAMON
void signal_handler(int signum) {
    switch (signum) {
        case SIGINT:
            end_program = 1;
            break;
        default:
            break;
    }
}
#endif

enum fan_mode { AUTO = 0, MANUAL = 1 };
enum fan_freq {
    LOW = 0,     // 2Hz
    MEDIUM = 1,  // 5Hz
    HIGH = 2,    // 10Hz
    MAX = 3      // 20Hz
};

// Sysfs helpers

static int sysfs_read_int(const char* path) {
    char buf[16];
    int f = open(path, O_RDONLY);
    if (f == -1) {
        perror(path);
        return -1;
    }
    ssize_t n = read(f, buf, sizeof(buf) - 1);
    close(f);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return atoi(buf);
}

static int sysfs_write_int(const char* path, int val) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", val);
    int f = open(path, O_WRONLY);
    if (f == -1) {
        perror(path);
        return -1;
    }
    write(f, buf, strlen(buf));
    close(f);
    return 0;
}

static enum fan_mode read_mode(void) {
    return (enum fan_mode)sysfs_read_int(MODE_PATH);
}
static enum fan_freq read_freq(void) {
    return (enum fan_freq)sysfs_read_int(FREQ_PATH);
}
static int read_temperature(void) { return sysfs_read_int(TEMP_PATH); }

static int write_mode(enum fan_mode mode) {
    if (mode != AUTO && mode != MANUAL) return -1;
    return sysfs_write_int(MODE_PATH, (int)mode);
}

static int write_freq(enum fan_freq freq) {
    if (freq < LOW || freq > MAX) return -1;
    return sysfs_write_int(FREQ_PATH, (int)freq);
}

// GPIO helpers

static int gpio_export(const char* gpio_num) {
    int f = open(GPIO_EXPORT, O_WRONLY);
    if (f == -1) {
        perror("gpio export");
        return -1;
    }
    write(f, gpio_num, strlen(gpio_num));
    close(f);
    return 0;
}

static int gpio_unexport(const char* gpio_num) {
    int f = open(GPIO_UNEXPORT, O_WRONLY);
    if (f == -1) {
        perror("gpio unexport");
        return -1;
    }
    write(f, gpio_num, strlen(gpio_num));
    close(f);
    return 0;
}

static int open_led(void) {
    gpio_unexport(GPIO_LED_NUM);
    gpio_export(GPIO_LED_NUM);

    int f = open(GPIO_LED_PATH "/direction", O_WRONLY);
    if (f == -1) {
        perror("led direction");
        return -1;
    }
    write(f, "out", 3);
    close(f);

    f = open(GPIO_LED_PATH "/value", O_RDWR);
    if (f == -1) perror("led value");
    return f;
}

static int open_button(const char* gpio_num) {
    char path[64];

    gpio_export(gpio_num);

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/direction", gpio_num);
    int f = open(path, O_WRONLY);
    if (f == -1) {
        perror("btn direction");
        return -1;
    }
    write(f, "in", 2);
    close(f);

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/edge", gpio_num);
    f = open(path, O_WRONLY);
    if (f == -1) {
        perror("btn edge");
        return -1;
    }
    write(f, "falling", 7);
    close(f);

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%s/value", gpio_num);
    f = open(path, O_RDONLY);
    if (f == -1) {
        perror("btn value");
        return -1;
    }

    // consume initial event
    char dummy;
    lseek(f, 0, SEEK_SET);
    read(f, &dummy, 1);

    return f;
}

void update_display(enum fan_freq freq, enum fan_mode mode) {
    const char* freq_str[] = {"Freq: 2Hz ", "Freq: 5Hz ", "Freq: 10Hz",
                              "Freq: 20Hz"};
    const char* mode_str[] = {"Mode: AUTO  ", "Mode: MANUAL"};
    char temperature[16];

    snprintf(temperature, sizeof(temperature), "Temp: %d C",
             read_temperature());
    ssd1306_set_position(0, 1);
    ssd1306_puts("Miniprojet - RD");
    ssd1306_set_position(0, 2);
    ssd1306_puts("--------------");
    ssd1306_set_position(0, 3);
    ssd1306_puts(temperature);
    ssd1306_set_position(0, 4);
    ssd1306_puts(freq_str[freq]);
    ssd1306_set_position(0, 5);
    ssd1306_puts(mode_str[mode]);
}

// Timer helper

static void timer_oneshot(int fd, struct itimerspec* spec, int ms) {
    spec->it_value.tv_sec = ms / 1000;
    spec->it_value.tv_nsec = (ms % 1000) * 1000000;
    spec->it_interval.tv_sec = 0;
    spec->it_interval.tv_nsec = 0;
    timerfd_settime(fd, 0, spec, NULL);
}

static void timer_periodic(int fd, struct itimerspec* spec, int ms) {
    spec->it_value.tv_sec = ms / 1000;
    spec->it_value.tv_nsec = (ms % 1000) * 1000000;
    spec->it_interval.tv_sec = ms / 1000;
    spec->it_interval.tv_nsec = (ms % 1000) * 1000000;
    timerfd_settime(fd, 0, spec, NULL);
}

// IPC command handler

static void handle_ipc_command(int client_fd, enum fan_freq* freq,
                               enum fan_mode* mode) {
    char buffer[256];
    char cmd[32], val[32];

    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        perror("ipc read");
        return;
    }
    buffer[n] = '\0';

    if (sscanf(buffer, "%31s %31s", cmd, val) != 2) {
        write(client_fd, "ERR Invalid format", 18);
        return;
    }

    if (strcmp(cmd, "mode") == 0) {
        int v = atoi(val);
        if (v != AUTO && v != MANUAL) {
            write(client_fd, "ERR Invalid mode value", 22);
            return;
        }
        *mode = (enum fan_mode)v;
        write_mode(*mode);
        write(client_fd, "OK", 2);

    } else if (strcmp(cmd, "freq") == 0) {
        if (*mode == AUTO) {
            write(client_fd, "ERR Cannot set freq in AUTO mode", 32);
            return;
        }
        int v = atoi(val);
        if (v < LOW || v > MAX) {
            write(client_fd, "ERR Invalid freq value", 22);
            return;
        }
        *freq = (enum fan_freq)v;
        write_freq(*freq);
        write(client_fd, "OK", 2);

    } else {
        write(client_fd, "ERR Unknown command", 19);
    }
}

int main() {
#if !DEAMON
    // Setup signal handler (not useful if only used as a systemd service, but
    // good for testing)
    static struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
#endif

    // IPC socket setup
    int ret;
    int socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }
    struct sockaddr_un sock_addr;
    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.sun_family = AF_UNIX;
    strncpy(sock_addr.sun_path, SOCKET_PATH, sizeof(sock_addr.sun_path) - 1);

    ret = bind(socket_fd, (struct sockaddr*)&sock_addr, sizeof(sock_addr));
    if (ret == -1) {
        perror("bind");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    ret = listen(socket_fd, 1);
    if (ret == -1) {
        perror("listen");
        close(socket_fd);
        return EXIT_FAILURE;
    }

    // Hardware setup
    ssd1306_init();
    int led = open_led();
    int buttons[] = {open_button(BTN_S1), open_button(BTN_S2),
                     open_button(BTN_S3)};

    // Timer setup
    int timer_led_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    int timer_screen_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_led_fd == -1 || timer_screen_fd == -1) {
        perror("timerfd_create");
        return EXIT_FAILURE;
    }
    struct itimerspec timer_led_spec = {0};
    struct itimerspec timer_screen_spec = {0};
    timer_periodic(timer_screen_fd, &timer_screen_spec, SCREEN_UPDATE_MS);

    // poll logic
    struct pollfd fds[NUM_FDS];

    for (int i = 0; i < NUM_BUTTONS; i++) {
        fds[i].fd = buttons[i];
        fds[i].events = POLLPRI;
    }
    fds[FD_SOCKET].fd = socket_fd;
    fds[FD_SOCKET].events = POLLIN;
    fds[FD_TIMER_LED].fd = timer_led_fd;
    fds[FD_TIMER_LED].events = POLLIN;
    fds[FD_TIMER_SCREEN].fd = timer_screen_fd;
    fds[FD_TIMER_SCREEN].events = POLLIN;

    // Initial state
    enum fan_freq freq = read_freq();
    enum fan_mode mode = read_mode();
    update_display(freq, mode);

    while (!end_program) {
        ret = poll(fds, (nfds_t)NUM_FDS, -1);

        if (ret == -1) {
            if (errno == EINTR) {
                // if we received a signal
                continue;
            }
            perror("poll");
            return EXIT_FAILURE;
        }

        // First button K1 - increase frequency
        if (fds[FD_BTN_S1].revents & POLLPRI) {
            // button i pressed
            char value;
            lseek(fds[FD_BTN_S1].fd, 0, SEEK_SET);
            read(fds[FD_BTN_S1].fd, &value, 1);
            if (mode == MANUAL && freq < MAX) {
                freq++;
                write_freq(freq);
            }
            pwrite(led, "1", 1, 0);
            timer_oneshot(timer_led_fd, &timer_led_spec, LED_FLASH_MS);
        }

        // Second button K2 - decrease frequency
        if (fds[FD_BTN_S2].revents & POLLPRI) {
            char value;
            lseek(fds[FD_BTN_S2].fd, 0, SEEK_SET);
            read(fds[FD_BTN_S2].fd, &value, 1);
            if (mode == MANUAL && freq > LOW) {
                freq--;
                write_freq(freq);
            }
            pwrite(led, "1", 1, 0);
            timer_oneshot(timer_led_fd, &timer_led_spec, LED_FLASH_MS);
        }

        // Third button K3 - change mode
        if (fds[FD_BTN_S3].revents & POLLPRI) {
            // button 2 pressed
            char value;
            lseek(fds[FD_BTN_S3].fd, 0, SEEK_SET);
            read(fds[FD_BTN_S3].fd, &value, 1);
            mode = (mode == AUTO) ? MANUAL : AUTO;
            write_mode(mode);
            pwrite(led, "1", 1, 0);
            timer_oneshot(timer_led_fd, &timer_led_spec, LED_FLASH_MS);
        }

        // LED flash timer expired — turn off
        if (fds[FD_TIMER_LED].revents & POLLIN) {
            uint64_t expirations;
            read(timer_led_fd, &expirations, sizeof(expirations));
            pwrite(led, "0", 1, 0);
        }

        // Screen refresh timer
        if (fds[FD_TIMER_SCREEN].revents & POLLIN) {
            uint64_t expirations;
            read(timer_screen_fd, &expirations, sizeof(expirations));
            if (mode == AUTO) {
                freq = read_freq();
            }
            update_display(freq, mode);
        }

        // IPC socket
        if (fds[FD_SOCKET].revents & POLLIN) {
            int client_fd = accept(socket_fd, NULL, NULL);
            if (client_fd != -1) {
                handle_ipc_command(client_fd, &freq, &mode);
                close(client_fd);
            }
        }
    }

    // Program used as deamon so this part should not be reached,
    // but in case it happens, clean up
    unlink(SOCKET_PATH);
    gpio_unexport(GPIO_LED_NUM);
    ssd1306_clear_display();
    ssd1306_set_position(0, 5);
    ssd1306_puts("Program ended..");

    printf("\nExiting program...\n");
    return 0;
}
