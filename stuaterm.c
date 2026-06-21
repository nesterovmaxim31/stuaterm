#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h> /* see /usr/include/bits/termios-*.h */
#include <bits/types.h> /* for __uint8_t */
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <poll.h>

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

static int f;

static void* main_loop_write(void* arg) {
    ssize_t user_input_length;
    size_t user_input_size = 0;
    char* user_input = NULL;
    int ret;

    __uint8_t term_data; /* два полубайта */
    unsigned char from_term_data[200];    

    while (1) {        
        /* read user input */
        user_input_length = getline(&user_input, &user_input_size, stdin);
        if (user_input_length == -1) {
            perror("User input error");
            goto out;
        }

        if (strcmp(user_input, "quit") == 0) {
            goto out;
        }

        /* write to terminal */
        term_data = 0; /* clear */
        term_data = user_input[0];

        ret = write(f, &term_data, 1);
        if (ret == -1) {
            perror("Failed to write to terminal");
            goto out;
        }
        else {
            puts("Wrote first char");
        }

        term_data = 0; /* clear */
        term_data = user_input[1];

        ret = write(f, &term_data, 1);
        if (ret == -1) {
            perror("Failed to write to terminal");
            goto out;
        }
        else {
            puts("Wrote second char");
        }

        /* send CR */
        term_data = 0x0D; 

        ret = write(f, &term_data, 1);
        if (ret == -1) {
            perror("Failed to write to terminal");
            goto out;
        }

        /* send LF */
        term_data = 0x0A;

        ret = write(f, &term_data, 1);
        if (ret == -1) {
            perror("Failed to write to terminal");
            goto out;
        }

        ret = tcdrain(f);
        if (ret == -1) {
            perror("tcdrain failed");
            goto out;
        }
        else
            puts("tcdrain successful");
    }

 out:
    free(user_input);
    return NULL;
}

static void* main_loop_read(void* arg) {
    unsigned char from_term_data[200];
    int ret;
    
    while (1) {
        bzero(from_term_data, 200);
        ret = read(f, from_term_data, 200);

        if (ret == -1)
            sleep(1);
        else
            printf("Read %d bytes: %s\n", ret, from_term_data);
    }
}



static void main_loop(int uart_fd) {
    struct pollfd fds[2];
    char buf[512], c, r = '\r';
    int pollret, ret;

    /* fds[0] - for stdin */
    /* fds[1] - for uart */
    fds[0].fd = 1;
    fds[0].events = POLLIN;

    fds[1].fd = uart_fd;
    fds[1].events = POLLIN;

    while (1) {
        pollret = poll(fds, 2, 100);

        /* Read smth from input and process */
        if (pollret != 0 && fds[0].revents != 0) {
            read(0, &c, 1);

            if (c == '\n')
                write(uart_fd, &r, 1);
            write(uart_fd, &c, 1);

            ret = tcdrain(uart_fd);
        }

        /* Read from uart and write to stdout */
        if (pollret != 0 && fds[1].revents != 0) {
            ret = read(uart_fd, buf, 512);
            write(1, buf, ret);
        }
    }
}

/*
  Disable canonical mode for input pseudoterminal,
  so we can read character as soon as they are typed
 */
#define stdin_nr 0
static struct termios stdin_old;

static int prepare_stdin() {
    struct termios stdin_new;
    int ret;
    
    ret = tcgetattr(0, &stdin_old);
    stdin_new = stdin_old;
    stdin_new.c_lflag = ~ICANON & stdin_new.c_lflag;

    /* Immediatly grub and retusn symbol */
    stdin_new.c_cc[VTIME] = 0; 
    stdin_new.c_cc[VMIN] = 0;

    ret = tcsetattr(0, TCSANOW, &stdin_new);

    return ret;
}

static int prepare_uart(int uart_fd) {
    struct termios f_termios_old, f_termios;
    int ret;
        /* Set term settings */
    ret = tcgetattr(f, &f_termios_old);
    if (ret == -1) {
        perror("tcgetattr failed");
        goto out;
    }

    f_termios = f_termios_old;
    cfmakeraw(&f_termios);
    
    set_2_stop_bits(&f_termios);
    set_mark_parity_bit(&f_termios);
    set_speed(&f_termios);
    set_8_bit_frame_size(&f_termios);

    ret = tcsetattr(f,  TCSANOW, &f_termios);
    if (ret == -1) {
        perror("Failed tcsetattr");
        goto out;
    }

 out:
    return ret;
}

static void restore_stdin_and_die(int ret) {
    tcsetattr(f, TCSANOW, &stdin_old);

    _exit(ret);
}

int main(int argc, char** argv) {
    struct termios f_termios, f_termios_old;
    int ret;
    

    /* open test */
    if (argc < 2) {
        puts("No terminal name specified");
        return -1;
    }

    f = open(argv[1], O_RDWR | O_NOCTTY | O_NDELAY | O_NONBLOCK);
    if (f == -1) {
        printf("Failed to open: %s", argv[1]);
        return -1;
    }
    else
        printf("%s opened!\n", argv[1]);
    
    if (isatty(f))
        printf("%s refers to a terminal\n", argv[1]);
    else {
        printf("%s isn't refered to a terminal", argv[1]);
        goto out;
    }

    prepare_stdin();
    prepare_uart(f);

    /* Init two threads */
    /* pthread_t t1, t2; */
    /* pthread_create(&t1, NULL, main_loop_write, NULL); */
    /* pthread_create(&t2, NULL, main_loop_read, NULL); */

    /* pthread_join(t1, NULL); */
    /* Restore term settings */
    /* ret = tcsetattr(f,  TCSANOW /\* immediat changes *\/, &f_termios_old); */
    /* if (ret == -1) { */
    /*     perror("Failed tcsetattr"); */
    /*     goto out; */
    /* }     */

    main_loop(f);
    
    close(f);
    restore_stdin_and_die(0);
 out:
    close(f);

    return 0;
}
