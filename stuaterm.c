#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h> /* see /usr/include/bits/termios-*.h */
#include <fcntl.h>
#include <string.h>
#include <poll.h>
#include <signal.h>

static int uart_fd;
static void restore_terms_settings_and_die(int ret);

/*
  CSTOPB - in control mode (termios-c_cflag.h)
 */
static void set_2_stop_bits(struct termios* f_termios) {
    f_termios->c_cflag = f_termios->c_cflag | CSTOPB;
}

/*
  PARENB - enable generation and detection of parity bit
  CMSPAR - mark parity bit
 */
static void set_mark_parity_bit(struct termios* f_termios) {
    f_termios->c_cflag = f_termios->c_cflag | PARENB;
    f_termios->c_cflag = f_termios->c_cflag | PARODD;
    f_termios->c_cflag = f_termios->c_cflag | CMSPAR;
}

/*
  B4800 - our baud rate
 */
static void set_speed(struct termios* f_termios) {
    cfsetispeed(f_termios, B4800);
    cfsetospeed(f_termios, B4800);
}

/*
  
 */
static void set_8_bit_frame_size(struct termios* f_termios) {
    f_termios->c_cflag = f_termios->c_cflag | CS8;
}

#define BUF_SIZE 512
#define POLL_TIMEOUT 100 /* in milliseconds */

static void main_loop() {
    struct pollfd fds[2];
    char buf[BUF_SIZE], c, r = '\r';
    int pollret, ret;

    /* fds[0] - for stdin */
    /* fds[1] - for uart */
    fds[0].fd = STDOUT_FILENO;
    fds[0].events = POLLIN;

    fds[1].fd = uart_fd;
    fds[1].events = POLLIN;

    while (1) {
        pollret = poll(fds, 2, POLL_TIMEOUT);

        /* Read smth from input and process */
        if (pollret != 0 && fds[0].revents != 0) {
            ret = read(STDIN_FILENO, &c, 1);
            if (ret == -1) {
                perror("read from stdin failed");
                restore_terms_settings_and_die(ret);
            }

            if (c == '\n')
                ret = write(uart_fd, &r, 1);
            ret = write(uart_fd, &c, 1);
            if (ret == -1) {
                perror("write to uart failed");
                restore_terms_settings_and_die(ret);
            }

            ret = tcdrain(uart_fd);
            if (ret == -1) {
                perror("tcdrain failed");
                restore_terms_settings_and_die(ret);
            }
        }

        /* Read from uart and write to stdout */
        if (pollret != 0 && fds[1].revents != 0) {
            if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                perror("uart is broken or disconnected");
                restore_terms_settings_and_die(ret);
            }
            
            ret = read(uart_fd, buf, BUF_SIZE);
            if (ret == -1) {
                perror("read from uart failed");
                restore_terms_settings_and_die(ret);
            }
            
            ret = write(STDOUT_FILENO, buf, ret);
            if (ret == -1) {
                perror("write to stdout failed");
                restore_terms_settings_and_die(ret);
            }
        }
    }
}

/*
  Disable canonical mode for input pseudoterminal,
  so we can read character as soon as they are typed
 */
static struct termios stdin_old;
static int prepare_stdin() {
    struct termios stdin_new;
    int ret;
    
    ret = tcgetattr(STDIN_FILENO, &stdin_old);
    if (ret != 0) {
        perror("STDIN preparation failed");
        restore_terms_settings_and_die(-1);
    }

    stdin_new = stdin_old;
    stdin_new.c_lflag = ~ICANON & stdin_new.c_lflag;

    /*
      Immediately grub any available symbol to process it
      (see 'info libc' for uncanonical mode)
     */
    stdin_new.c_cc[VTIME] = 0; 
    stdin_new.c_cc[VMIN] = 0;

    ret = tcsetattr(STDIN_FILENO, TCSANOW, &stdin_new);
    if (ret != 0) {
        perror("STDIN preparation failed");
        restore_terms_settings_and_die(-1);
    }

    return ret;
}

static struct termios uart_termios_old;
static void prepare_uart() {
    struct termios uart_termios;
    int ret;

    /* Set term settings */
    ret = tcgetattr(uart_fd, &uart_termios_old);
    if (ret == -1) {
        perror("tcgetattr failed on uart");
        restore_terms_settings_and_die(ret);
    }

    uart_termios = uart_termios_old;
    cfmakeraw(&uart_termios);
    
    set_2_stop_bits(&uart_termios);
    set_mark_parity_bit(&uart_termios);
    set_speed(&uart_termios);
    set_8_bit_frame_size(&uart_termios);

    uart_termios.c_cc[VTIME] = 0;
    uart_termios.c_cc[VMIN] = 0;

    ret = tcsetattr(uart_fd, TCSANOW, &uart_termios);
    if (ret == -1) {
        perror("tcsetattr failed on uart");
        restore_terms_settings_and_die(ret);
    }
}

static void restore_terms_settings_and_die(int ret) {
    tcsetattr(STDIN_FILENO, TCSANOW, &stdin_old);

    if (uart_fd != -1) {
        tcsetattr(uart_fd, TCSANOW, &uart_termios_old);
        close(uart_fd);
    }

    _exit(ret);
}

static int open_uart(const char* path) {
    uart_fd = open(path, O_RDWR | O_NOCTTY | O_NDELAY | O_NONBLOCK);

    if (uart_fd == -1) {
        printf("Failed to open: %s", path);
        return -1;
    }
}

static void sigint_handler(int sign) {
    puts("Exit stuaterm");
    restore_terms_settings_and_die(0);
}

static void register_sigint_handler() {
    struct sigaction sa;

    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Failed to register Ctrl-C handler");
        _exit(-1);
    }        
}

int main(int argc, char** argv) {
    uart_fd = -1;
 
    /* open test */
    if (argc < 2) {
        puts("No terminal name specified");
        return -1;
    }

    register_sigint_handler();

    prepare_stdin();

    uart_fd = open_uart(argv[1]);
    if (uart_fd == -1) {
        printf("Failed to open: %s", argv[1]);
        return -1;
    }
    else
        printf("%s opened successfully\n------------------\n", argv[1]);
    
    if (!isatty(uart_fd)) {
        printf("%s isn't refered to a terminal", argv[1]);
        restore_terms_settings_and_die(-1);
    }

    prepare_uart();

    main_loop();
}
