/* 
 * This file is part of the stuaterm (https://github.com/).
 * Copyright (c) 2026 Maksim Nesterov.
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <termios.h> /* see /usr/include/bits/termios-*.h */
#include <fcntl.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <getopt.h>

static int uart_fd;

#define BUF_SIZE 512
#define POLL_TIMEOUT 100 /* in milliseconds */
#define ENTRYSIZE 64
#define ENTRYSIZE_ST (ENTRYSIZE + 1) /* we need \0 in the end */
static struct termios stdin_old;
static struct termios uart_termios_old;
static struct {
    char* path; /* NULL (default) */
    char speed[ENTRYSIZE_ST]; /* "9600" (default) */
    char stopbit[ENTRYSIZE_ST]; /* "one" (default), "two" */
    char paritybit[ENTRYSIZE_ST]; /* "none" (default), "odd", "even",
                       "mark", "space" */
    char databits[ENTRYSIZE_ST]; /* "5", "6", "7", "8" (default) */
    char inputparitycheck[ENTRYSIZE_ST]; /* "disable" (default),
                              "enable_ignore", "enable_mark" */
    char replacenluser[ENTRYSIZE_ST]; /* "" (default) */
    char replacenldevice[ENTRYSIZE_ST]; /* "" (default) */
    char replacecruser[ENTRYSIZE_ST]; /* "" (default) */
    char replacecrdevice[ENTRYSIZE_ST]; /* "" (default) */
} arguments;
/* Should be checked bytes from device on errors marks
   (inputparitycheck = enable_mark) */
static int flag_check_parity_error;

static void restore_terms_settings_and_die(int ret) {
    tcsetattr(STDIN_FILENO, TCSANOW, &stdin_old);

    if (uart_fd != -1) {
        tcsetattr(uart_fd, TCSANOW, &uart_termios_old);
        close(uart_fd);
    }

    _exit(ret);
}

static void main_loop() {
    struct pollfd fds[2];
    char b, c, r = '\r';
    int pollret, ret;
    int first_error_byte = 0, second_error_byte = 0;
    

    /* fds[0] - for stdin */
    /* fds[1] - for uart */
    fds[0].fd = STDIN_FILENO;
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
            
            ret = read(uart_fd, &b, 1);
            if (ret == -1) {
                perror("read from uart failed");
                restore_terms_settings_and_die(ret);
            }

            /* Handle marked byte with parity error */
            if (flag_check_parity_error == 1) {
                if (first_error_byte == 0 && b == 0xFF) {
                    first_error_byte = 1;
                    continue;
                }
                else if (first_error_byte == 1 &&
                         second_error_byte == 0 &&
                         b == 0) {
                    second_error_byte = 1;
                    continue;
                }
                else if (first_error_byte == 1 &&
                         second_error_byte == 0 &&
                         b != 0) {
                    first_error_byte = 0;
                    continue;
                }

                else if (first_error_byte == 1 &&
                         second_error_byte == 1) {
                    printf("parity or framing error is detected!\nBroken received byte: ");
                    ret = write(STDOUT_FILENO, &b, 1);
                    if (ret == -1) {
                        perror("write to stdout failed");
                        restore_terms_settings_and_die(ret);
                    }       

                    first_error_byte = 0;
                    second_error_byte = 0;
                    continue;
                }
                else {
                    first_error_byte = 0;
                    second_error_byte = 0;
                }
            }
            
            ret = write(STDOUT_FILENO, &b, 1);
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
static void prepare_stdin() {
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
}

static int open_uart(const char* path) {
    int ret;

    ret = open(path, O_RDWR | O_NOCTTY | O_NDELAY | O_NONBLOCK);

    if (ret == -1) {
        printf("Failed to open: %s", path);
        return -1;
    }

    return ret;
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

/* Parse speed value from optarg or config file and set value in
   termios struct */
static void set_speed(struct termios* uart_termios,
                      const char* optarg) {
    int speed = atoi(optarg);
    if (speed == 0) {
        printf("Speed value: %s is unrecognized\n", optarg);
        perror("");
        restore_terms_settings_and_die(speed);
    }

    switch (speed) {
    case 0:
        cfsetispeed(uart_termios, B0);
        cfsetospeed(uart_termios, B0);
        return;
    case 50:
        cfsetispeed(uart_termios, B50);
        cfsetospeed(uart_termios, B50);
        return;
    case 75:
        cfsetispeed(uart_termios, B75);
        cfsetospeed(uart_termios, B75);
        return;
    case 110:
        cfsetispeed(uart_termios, B110);
        cfsetospeed(uart_termios, B110);
        return;
    case 134:
        cfsetispeed(uart_termios, B134);
        cfsetospeed(uart_termios, B134);
        return;
    case 150:
        cfsetispeed(uart_termios, B150);
        cfsetospeed(uart_termios, B150);
        return;
    case 200:
        cfsetispeed(uart_termios, B200);
        cfsetospeed(uart_termios, B200);
        return;
    case 300:
        cfsetispeed(uart_termios, B300);
        cfsetospeed(uart_termios, B300);
        return;
    case 600:
        cfsetispeed(uart_termios, B600);
        cfsetospeed(uart_termios, B600);
        return;
    case 1200:
        cfsetispeed(uart_termios, B1200);
        cfsetospeed(uart_termios, B1200);
        return;
    case 1800:
        cfsetispeed(uart_termios, B1800);
        cfsetospeed(uart_termios, B1800);
        return;
    case 2400:
        cfsetispeed(uart_termios, B2400);
        cfsetospeed(uart_termios, B2400);
        return;
    case 4800:
        cfsetispeed(uart_termios, B4800);
        cfsetospeed(uart_termios, B4800);
        return;
    case 9600:
        cfsetispeed(uart_termios, B9600);
        cfsetospeed(uart_termios, B9600);
        return;
    case 19200:
        cfsetispeed(uart_termios, B19200);
        cfsetospeed(uart_termios, B19200);
        return;
    case 38400:
        cfsetispeed(uart_termios, B38400);
        cfsetospeed(uart_termios, B38400);
        return;
    default:
        printf("Speed value: %d isn't supported. Check libc documentation", \
               speed);
        restore_terms_settings_and_die(-1);
    }
}

/* Parse stopbit value from optarg or config file and set value in
   termios struct */
static void set_stopbit(struct termios* uart_termios,
                        const char* optarg) {
    if (strncmp("one", optarg, 3) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag & (~CSTOPB);/* сложно */
    }
    /* If CSTOPB is set, two stop bits are used */
    else if (strncmp("two", optarg, 3) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag | CSTOPB;
    }
    else {
        printf("Stop bit: %s is unrecognized\n", optarg);
        restore_terms_settings_and_die(-1);
    }

}

/* Parse paritybit value from optarg or config file and set value in
   termios struct */
static void set_paritybit(struct termios* uart_termios,
                          const char* optarg) {
    /* If PARENB is not set, than no parity bit generate and check */
    if (strncmp("none", optarg, 4) == 0)  {
        uart_termios->c_cflag = uart_termios->c_cflag & (~PARENB);
    }
    /* If PARODD is set, than odd parity is used  */
    else if (strncmp("odd", optarg, 3) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag | PARENB;
        uart_termios->c_cflag = uart_termios->c_cflag | PARODD;
        /* disable stick parity bit */
        uart_termios->c_cflag = uart_termios->c_cflag & (~CMSPAR); 
    }
    else if (strncmp("even", optarg, 4) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag | PARENB;
        uart_termios->c_cflag = uart_termios->c_cflag & (~PARODD);
        uart_termios->c_cflag = uart_termios->c_cflag & (~CMSPAR); 
    }
    /* If CMSPAR is set, than mark parity is used */
    else if (strncmp("mark", optarg, 4) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag | PARENB;
        uart_termios->c_cflag = uart_termios->c_cflag | PARODD;
        uart_termios->c_cflag = uart_termios->c_cflag | CMSPAR; 
    }
    else if (strncmp("space", optarg, 5) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag | PARENB;
        uart_termios->c_cflag = uart_termios->c_cflag & (~PARODD);
        uart_termios->c_cflag = uart_termios->c_cflag | CMSPAR; 
    }
    else {
        printf("Parity bit type: %s is unrecognized\n", optarg);
        restore_terms_settings_and_die(-1);
    }    
}

/* Set amount of data bits in frame: 5, 6, 7 or 8 bits.
   Currently in glibc other amount of data bits aren't avalaible.
   If fact it sets how many bits is reserved in frame for
   data (payload).
*/
static void set_databits(struct termios* uart_termios,
                          const char* optarg) {

    int size = atoi(optarg);
    if (size == 0) {
        printf("Data bits: %s is unrecognized", optarg);
        perror("");
        restore_terms_settings_and_die(-1);
    }

    /* Clear CSIZE field */
    uart_termios->c_cflag = uart_termios->c_cflag & (~CSIZE);

    switch (size) {
    case 5:
        uart_termios->c_cflag = uart_termios->c_cflag | CS5;
        break;

    case 6:
        uart_termios->c_cflag = uart_termios->c_cflag | CS6;
        break;

    case 7:
        uart_termios->c_cflag = uart_termios->c_cflag | CS7;
        break;
    case 8:
        uart_termios->c_cflag = uart_termios->c_cflag | CS8;
        break;
    default:
        printf("%d data bits is unsupported\n", size);
        restore_terms_settings_and_die(-1);
    }
}

/*
  disable - no parity checking is done at all on input frames
  enable_ignore - any bytes with framing or parity error are ignored
  enable_mark - any bytes with framing or parity error are marked
  with preceding bytes '377' and '0' (This should be checked in main
  loop). (ISTRIP is not used at all)
 */
static void set_inputparitycheck(struct termios* uart_termios,
                                 const char* optarg) {
    if (strcmp("disable", optarg) == 0) {
        uart_termios->c_iflag = uart_termios->c_iflag & (~INPCK);
    }
    else if (strcmp("enable_ignore", optarg) == 0) {
        uart_termios->c_iflag = uart_termios->c_iflag | INPCK;
        uart_termios->c_iflag = uart_termios->c_iflag | IGNPAR;
        uart_termios->c_iflag = uart_termios->c_iflag & (~PARMRK);
    }
    else if (strcmp("enable_mark", optarg) == 0) {
        uart_termios->c_iflag = uart_termios->c_iflag | INPCK;
        uart_termios->c_iflag = uart_termios->c_iflag & (~IGNPAR);
        uart_termios->c_iflag = uart_termios->c_iflag | PARMRK;

        flag_check_parity_error = 1;
    }
    else {
        /* TODO: add available options */
        printf("inputparitycheck option: %s is unrecognized\n",
               optarg);
    }
}
/* Find symbol #, and everythings after this symbol is erased
   (# is replaced with \0)
*/
static void erase_comments(char* line) {
    char* commsign = strchr(line, '#');

    if (commsign != NULL)
        *commsign = '\0';
}

/*
  Config file looks like (spaces can be in random places):
  speed=<some speed>
  stopbit=<stop bit type>
  paritybit = <parity bit type>

  We should parse file line by line, then each line break with strtok_r

  If function return -1, then file doesn't exist. If some other error
  with parsing or file opening function calls restore_terms_settings_and_die
*/
static int parse_config_file(struct termios* uart_termios,
                             const char* path) {
    FILE* f; /* I so hate this ugly F I L E */
    char* line = NULL, *saveptr, *key, *value;
    ssize_t line_size;
    size_t line_capacity;

    f = fopen(path, "r");
    if (f == NULL) {
        /* File doesn't exist */
        if (errno == ENOENT) {
            return -1;
        }
        else {
            printf("Failed to open file: %s", path);
            perror("");
            restore_terms_settings_and_die(errno);
        }
    }
    /* File is opened successfully, try to parse it */
    else {
        do {
            line_size = getline(&line, &line_capacity, f);
            if (line_size == -1) {
                break;
                free(line);
                perror("Config file parsing failed");
                restore_terms_settings_and_die(-1);
            }

            erase_comments(line);

            key = strtok_r(line, "= \n", &saveptr);
            if (key == NULL)
                continue; /* get new line */

            value = strtok_r(NULL, "= \n", &saveptr);
            if (value == NULL) {
                printf("No value is set in config file for key '%s'\n", key);
                free(line);
                restore_terms_settings_and_die(-1);
            }            

            if (strcmp(key, "speed") == 0) {
                strncpy(arguments.speed, value, ENTRYSIZE);
            }
            else if (strcmp(key, "stopbit") == 0) {
                strncpy(arguments.stopbit, value, ENTRYSIZE);
            }
            else if (strcmp(key, "paritybit") == 0) {
                strncpy(arguments.paritybit, value, ENTRYSIZE);
            }
            else if (strcmp(key, "databits") == 0) {
                strncpy(arguments.databits, value, ENTRYSIZE);
            }
            else if (strcmp(key, "inputparitycheck") == 0) {
                strncpy(arguments.inputparitycheck, value, ENTRYSIZE);
            }
        } while (line_size != -1);

        free(line);
    }

    fclose(f);
    
    return 0;
}

/* Set on serial device settings from 'arguments' */
static void prepare_uart(struct termios* uart_termios) {
    int ret;

    /* If user's file doesn't exist -> exit with error  */
    ret = parse_config_file(uart_termios, arguments.path);
    if (ret == -1) {
        printf("File: %s doesn't exist", arguments.path);
        restore_terms_settings_and_die(-1);
    }

    set_speed(uart_termios, arguments.speed);
    set_stopbit(uart_termios, arguments.stopbit);
    set_paritybit(uart_termios, arguments.paritybit);
    set_databits(uart_termios, arguments.databits);
    set_inputparitycheck(uart_termios, arguments.inputparitycheck);
    
    uart_termios->c_cc[VTIME] = 0;
    uart_termios->c_cc[VMIN] = 0;

    /* setting settings */
    ret = tcsetattr(uart_fd, TCSANOW, uart_termios);
    if (ret == -1) {
        perror("tcsetattr failed on uart");
        restore_terms_settings_and_die(ret);
    }
}

/*
  Arguments (options, everyone name it how he wants):
  --config           (-c) (path to config file)
  --speed            (-s) (see libc for available values)
  --stopbit          (-b) (one, two)
  --paritybit        (-p) (none, odd, even, mark, space)
  --databits         (-d) (5, 6, 7, 8)
  --inputparitycheck (-i) (disable, enable_ignore, enable_mark)
  --replacenluser    (-n) (some set of character, for example - \r\n)
  --replacenldevice  (-o) (some set of character)
  --replacecruser    (-r) (some set of character)
  --replacecrdevice  (-t) (some set of character)

  return:
  -1 - no arguments was provied (so we try to open config files on
  standart place)
  0 - some arguments was provided
 */
static int parse_args(const int argc, char** argv) {
    int opt, flag = 0, l;

    static struct option long_options[] = {
        {"config",           required_argument, NULL, 'c'},
        {"speed",            required_argument, NULL, 's'},
        {"stopbit",          required_argument, NULL, 'b'},
        {"paritybit",        required_argument, NULL, 'p'},
        {"databits",         required_argument, NULL, 'd'},
        {"inputparitycheck", required_argument, NULL, 'i'},
        {"replacenluser",    required_argument, NULL, 'n'},
        {"replacenldevice",  required_argument, NULL, 'o'},
        {0, 0, 0, 0}
    };

    /* We believe that getopt_long checked, that optarg is given by
       user (in long_optiongs 'required_argument' is used) */
    while ((opt = getopt_long(argc, argv, "c:s:b:p:d:i:n:o:", long_options,
                              &l)) != -1) {
        flag = 1; /* So we get some argument */

        switch (opt) {
        case 'c':
            arguments.path = optarg;
            break;

        case 's':
            strncpy(arguments.speed, optarg, ENTRYSIZE);
            break;

        case 'b':
            strncpy(arguments.stopbit, optarg, ENTRYSIZE);
            break;

        case 'p':
            strncpy(arguments.paritybit, optarg, ENTRYSIZE);
            break;

        case 'd':
            strncpy(arguments.databits, optarg, ENTRYSIZE);
            break;

        case 'i':
            strncpy(arguments.inputparitycheck, optarg, ENTRYSIZE);
            break;

        case 'n':
            strncpy(arguments.replacenluser, optarg, ENTRYSIZE);
            break;

        case 'o':
            strncpy(arguments.replacenldevice, optarg, ENTRYSIZE);
            break;

        }
    }

    return flag ? 0 : -1;
}

/*
  Firstly of all we parse arguments, if we get -c then parse config file.
  If we have not enough arguments we use settings, that was installed
  on uart previously, or by default. (If we have argument -c with
  other arguments it can be dangerous).

  Secondly, if no arguments are provided at all, we try to open
  config files on standart places:
  1. stuaterm.conf (cwd)
  2. /etc/stuaterm.conf
*/
int main(int argc, char** argv) {
    struct termios uart_termios;
    int ret;
    
    uart_fd = -1;

    flag_check_parity_error = 0;
 
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

    /* Save term settings */
    ret = tcgetattr(uart_fd, &uart_termios_old);
    if (ret == -1) {
        perror("tcgetattr failed on uart");
        restore_terms_settings_and_die(ret);
    }

    uart_termios = uart_termios_old;

    /* Clear all settings before setting new */
    cfmakeraw(&uart_termios);

    /* Setting default values for arguments */
    arguments.path = NULL;
    strcpy(arguments.speed, "9600");
    strcpy(arguments.stopbit, "one");
    strcpy(arguments.paritybit, "none");
    strcpy(arguments.databits, "8");
    strcpy(arguments.inputparitycheck, "disable");
    strcpy(arguments.replacenluser, "");
    strcpy(arguments.replacenldevice, "");
    strcpy(arguments.replacecruser, "");
    strcpy(arguments.replacecrdevice, "");
    
    ret = parse_args(argc, argv);
    printf("parsed %d\n", ret);
    if (ret == -1) {/* No arguments was provided, so try to open
                      some standart config files */
        ret = parse_config_file(&uart_termios, "stuaterm.conf");
        if (ret == -1) { /* Try another place */
            ret = parse_config_file(&uart_termios, "/etc/stuaterm.conf");
            if (ret == -1) {
                puts("No config arguments were provide. stuaterm will \
 works with current settings on uart device. (You can check them with stty)");
            }
        }
    }

    prepare_uart(&uart_termios);

    main_loop();
}
