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

#include "../oled/ssd1306.h"

#define GPIO_EXPORT "/sys/class/gpio/export"
#define GPIO_UNEXPORT "/sys/class/gpio/unexport"
#define GPIO_LED "/sys/class/gpio/gpio362"
#define LED "362"
#define NUM_BUTTONS 3
#define NUM_FDS \
    (NUM_BUTTONS + 3)  // +3 for the LED control timer and screen update timer,
                       // and the socket for receiving commands

#define MODE_PATH "/sys/class/fan_module_class/fan_module_device/mode"
#define FREQ_PATH "/sys/class/fan_module_class/fan_module_device/freq"
#define TEMP_PATH "/sys/class/fan_module_class/fan_module_device/temp"
#define SOCKET_PATH "/tmp/fan.sock"

int end_program;

void signal_handler(int signum) {
    const char* msg = NULL;
    switch (signum) {
        case SIGINT:
            end_program = 1;
            break;
        default:
            msg = "Received unknown signal, ignoring...\n";
    }
}

enum fan_mode { AUTO = 0, MANUAL = 1 };
enum fan_freq {
    LOW = 0,     // 2Hz
    MEDIUM = 1,  // 5Hz
    HIGH = 2,    // 10Hz
    MAX = 3      // 20Hz
};

static enum fan_mode read_mode() {
    char buf[16];
    int f = open(MODE_PATH, O_RDONLY);
    if (f == -1) {
        perror("Failed to open mode file");
        return -1;
    }
    read(f, buf, sizeof(buf));
    close(f);
    return (enum fan_mode)atoi(buf);
}

static int write_mode(enum fan_mode mode) {
    if (mode != AUTO && mode != MANUAL) {
        fprintf(stderr, "Invalid mode value: %d\n", mode);
        return -1;
    }

    char buf[16];
    sprintf(buf, "%d", mode);
    int f = open(MODE_PATH, O_WRONLY);
    if (f == -1) {
        perror("Failed to open mode file");
        return -1;
    }
    write(f, buf, strlen(buf));
    close(f);
    return 0;
}

static enum fan_freq read_freq() {
    char buf[16];
    int f = open(FREQ_PATH, O_RDONLY);
    if (f == -1) {
        perror("Failed to open freq file");
        return -1;
    }
    read(f, buf, sizeof(buf));
    close(f);
    return (enum fan_freq)atoi(buf);
}

static int write_freq(enum fan_freq freq) {
    if (freq < LOW || freq > MAX) {
        fprintf(stderr, "Invalid frequency value: %d\n", freq);
        return -1;
    }

    char buf[16];
    sprintf(buf, "%d", freq);
    int f = open(FREQ_PATH, O_WRONLY);
    if (f == -1) {
        perror("Failed to open freq file");
        return -1;
    }
    write(f, buf, strlen(buf));
    close(f);
    return 0;
}

static int open_led() {
    // unexport pin out of sysfs (reinitialization)
    int f = open(GPIO_UNEXPORT, O_WRONLY);

    write(f, LED, strlen(LED));
    close(f);

    // export pin to sysfs
    f = open(GPIO_EXPORT, O_WRONLY);
    write(f, LED, strlen(LED));
    close(f);

    // config pin
    f = open(GPIO_LED "/direction", O_WRONLY);
    write(f, "out", 3);
    close(f);

    // open gpio value attribute
    f = open(GPIO_LED "/value", O_RDWR);
    return f;
}

static int open_button(const char* gpio) {
    int f;

    // export
    f = open(GPIO_EXPORT, O_WRONLY);
    write(f, gpio, strlen(gpio));
    close(f);

    // direction = input
    char path[100];
    sprintf(path, "/sys/class/gpio/gpio%s/direction", gpio);
    f = open(path, O_WRONLY);
    write(f, "in", 2);
    close(f);

    // edge = both (ou rising/falling)
    sprintf(path, "/sys/class/gpio/gpio%s/edge", gpio);
    f = open(path, O_WRONLY);
    write(f, "falling", 7);
    close(f);

    // open value
    sprintf(path, "/sys/class/gpio/gpio%s/value", gpio);
    f = open(path, O_RDONLY);

    char dummy;
    lseek(f, 0, SEEK_SET);
    read(f, &dummy, 1);

    return f;
}

static int read_temperature() {
    char buf[16];
    int f = open(TEMP_PATH, O_RDONLY);
    if (f == -1) {
        perror("Failed to open freq file");
        return -1;
    }
    read(f, buf, sizeof(buf));
    close(f);
    return atoi(buf);
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

int main() {
    // Initialisation
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

    static struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    end_program = 0;
    ssd1306_init();
    int led = open_led();
    int buttons[] = {open_button("0"), open_button("2"), open_button("3")};

    int timer_led_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_led_fd == -1) {
        perror("timerfd_create led");
        return EXIT_FAILURE;
    }
    struct itimerspec timer_led_spec = {0};

    int timer_screen_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_screen_fd == -1) {
        perror("timerfd_create screen");
        return EXIT_FAILURE;
    }
    struct itimerspec timer_screen_spec = {0};

    // poll logic
    struct pollfd fds[NUM_FDS];
    // Poll buttons for user input
    for (int i = 0; i < NUM_BUTTONS; i++) {
        fds[i].fd = buttons[i];
        fds[i].events = POLLPRI;
    }
    fds[NUM_FDS - 2].fd = timer_led_fd;  // Add timer control to poll
    fds[NUM_FDS - 2].events = POLLIN;

    fds[NUM_FDS - 1].fd = timer_screen_fd;  // Add screen update timer to poll
    fds[NUM_FDS - 1].events = POLLIN;

    fds[NUM_FDS - 3].fd = socket_fd;  // Add socket to poll
    fds[NUM_FDS - 3].events = POLLIN;

    timer_led_spec.it_value.tv_sec = 0;
    timer_led_spec.it_value.tv_nsec = 50 * 1000000;  // 100ms
    timer_led_spec.it_interval.tv_sec = 0;
    timer_led_spec.it_interval.tv_nsec = 0;  // One-shot timer

    timerfd_settime(timer_led_fd, 0, &timer_led_spec, NULL);

    timer_screen_spec.it_value.tv_sec = 0;
    timer_screen_spec.it_value.tv_nsec = 500 * 1000000;  // 500ms
    timer_screen_spec.it_interval.tv_sec = 0;
    timer_screen_spec.it_interval.tv_nsec = 500 * 1000000;  // Periodic timer
    timerfd_settime(timer_screen_fd, 0, &timer_screen_spec, NULL);

    enum fan_freq freq = read_freq();
    enum fan_mode mode = read_mode();
    printf("Initial mode: %s, freq: %d\n", (mode == AUTO) ? "AUTO" : "MANUAL",
           freq);
    while (!end_program) {
        int ret = poll(fds, (nfds_t)NUM_FDS, -1);

        if (ret == -1) {
            if (errno == EINTR) {
                // if we received a signal
                continue;
            }
            perror("poll");
            return EXIT_FAILURE;
        }

        if (fds[0].revents & POLLPRI) {
            // button i pressed
            char value;
            lseek(fds[0].fd, 0, SEEK_SET);
            read(fds[0].fd, &value, 1);
            if (mode == MANUAL) {
                if (freq < MAX) {
                    freq++;
                } else {
                    freq = MAX;
                }
                write_freq(freq);
                pwrite(led, "1", 1, 0);
                timerfd_settime(timer_led_fd, 0, &timer_led_spec, NULL);
            }
        }

        if (fds[1].revents & POLLPRI) {
            // button 1 pressed
            char value;
            lseek(fds[1].fd, 0, SEEK_SET);
            read(fds[1].fd, &value, 1);
            if (mode == MANUAL) {
                if (freq > LOW) {
                    freq--;
                } else {
                    freq = LOW;
                }
                write_freq(freq);
                pwrite(led, "1", 1, 0);
                timerfd_settime(timer_led_fd, 0, &timer_led_spec, NULL);
            }
        }

        if (fds[2].revents & POLLPRI) {
            // button 2 pressed
            char value;
            lseek(fds[2].fd, 0, SEEK_SET);
            read(fds[2].fd, &value, 1);
            mode = (mode == AUTO) ? MANUAL : AUTO;
            write_mode(mode);
            pwrite(led, "1", 1, 0);
            timerfd_settime(timer_led_fd, 0, &timer_led_spec, NULL);
        }
        if (fds[NUM_FDS - 2].revents & POLLIN) {
            uint64_t expirations;
            read(timer_led_fd, &expirations, sizeof(expirations));
            pwrite(led, "0", 1, 0);
        }
        if (fds[NUM_FDS - 1].revents & POLLIN) {
            uint64_t expirations;
            read(timer_screen_fd, &expirations, sizeof(expirations));
            if (mode == AUTO) {
                freq = read_freq();
            }
            update_display(freq, mode);
        }

        if (fds[NUM_FDS - 3].revents & POLLIN) {
            printf("Received command on socket\n");
            int client_fd = accept(socket_fd, NULL, NULL);
            if (client_fd == -1) {
                perror("accept");
                continue;
            }
            printf("Client connected\n");

            char buffer[256];
            char cmd[32], val[32];
            ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
            if (n == -1) {
                perror("read");
                close(client_fd);
                continue;
            }
            buffer[n] = '\0';
            if (sscanf(buffer, "%s %s", cmd, val) == 2) {
                printf("Received command: %s %s\n", cmd, val);
                if (strcmp(cmd, "mode") == 0) {
                    mode = atoi(val);

                    if (mode != AUTO && mode != MANUAL) {
                        fprintf(stderr, "Invalid mode value: %d\n", mode);
                        write(client_fd, "Invalid mode value", 18);
                        close(client_fd);
                        continue;
                    }
                    write_mode(mode);
                    write(client_fd, "OK", 2);
                } else if (strcmp(cmd, "freq") == 0) {
                    freq = atoi(val);

                    if (freq < LOW || freq > MAX) {
                        fprintf(stderr, "Invalid frequency value: %d\n", freq);
                        write(client_fd, "Invalid frequency value", 22);
                        close(client_fd);
                        continue;
                    }

                    if (mode == AUTO) {
                        write(client_fd, "Cannot set frequency in AUTO mode",
                              33);
                        close(client_fd);
                        continue;
                    }
                    write_freq(freq);
                    write(client_fd, "OK", 2);
                } else {
                    write(client_fd, "Unknown command", 15);
                }
            }
            close(client_fd);
        }
    }

    // Program used as deamon so this part should not be reached,
    // but in case it happens, clean up
    unlink(SOCKET_PATH);
    ssd1306_clear_display();
    ssd1306_set_position(0, 5);
    ssd1306_puts("Program ended..");

    printf("\nExiting program...\n");
    return 0;
}
