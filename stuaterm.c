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
static void prepare_uart(struct termios* uart_termios) {
    int ret;

    cfmakeraw(uart_termios);
    
    //    set_2_stop_bits(&uart_termios);
    //    set_mark_parity_bit(&uart_termios);
    //    set_speed(&uart_termios);
    set_8_bit_frame_size(uart_termios);

    uart_termios->c_cc[VTIME] = 0;
    uart_termios->c_cc[VMIN] = 0;

    ret = tcsetattr(uart_fd, TCSANOW, uart_termios);
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

/* Parse speed value from optarg or config file and set value in
   termios struct */
static void set_speed(struct termios* uart_termios, const char* optarg) {
    int speed = atoi(optarg);
    if (speed != 0) {
        printf("Speed value: %s is unrecognized", optarg);
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
static void set_stopbit(struct termios* uart_termios, const char* optarg) {
    if (strncmp("one", optarg, 3) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag ^ (~CSTOPB);/* сложно */
        /* ^ is XOR by the way */
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
static void set_paritybit(struct termios* uart_termios, const char* optarg) {
    /* If PARENB is not set, than no parity bit generate and check */
    if (strncmp("none", optarg, 4) == 0)  {
        uart_termios->c_cflag = uart_termios->c_cflag ^ (~PARENB);
    }
    /* If PARODD is set, than odd parity is used  */
    else if (strncmp("odd", optarg, 3) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag | PARENB;
        uart_termios->c_cflag = uart_termios->c_cflag | PARODD;
    }
    else if (strncmp("even", optarg, 4) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag | PARENB;
        uart_termios->c_cflag = uart_termios->c_cflag ^ (~PARODD);
    }
    /* If CMSPAR is set, than mark parity is used */
    else if (strncmp("mark", optarg, 4) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag | PARENB;
        uart_termios->c_cflag = uart_termios->c_cflag | PARODD;
        uart_termios->c_cflag = uart_termios->c_cflag | CMSPAR; 
    }
    else if (strncmp("space", optarg, 5) == 0) {
        uart_termios->c_cflag = uart_termios->c_cflag | PARENB;
        uart_termios->c_cflag = uart_termios->c_cflag | PARODD;
        uart_termios->c_cflag = uart_termios->c_cflag ^ (~CMSPAR); 
    }
    else {
        printf("Parity bit type: %s is unrecognized\n", optarg);
        restore_terms_settings_and_die(-1);
    }    
}

/* Find symbol #, and everythings after this symbol is erased (# is replaced
   with \0)
*/
static void erase_comments(char* line) {
    char* commsign = strchr(line, '#');
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
static int parse_config_file(struct termios* uart_termios, const char* path) {
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
                set_speed(uart_termios, value);
            }
            else if (strcmp(key, "stopbit") == 0) {
                set_stopbit(uart_termios, value);
            }
            else if (strcmp(key, "paritybit") == 0) {
                set_paritybit(uart_termios, value);
            }
        } while (line_size != -1);

        free(line);
    }

    return 0;
}

/*
  Arguments:
  --config    (-c) (path to config file)
  --speed     (-s) (see libc for available values)
  --stopbit   (-b) (one, two)
  --paritybit (-p) (none, odd, even, mark, space)

  return:
  -1 - no arguments was provied (so we try to open config files on standart
  place)
  0 - some arguments was provided
 */
static int parse_args(const int argc, char** argv,
                       struct termios* uart_termios) {
    int opt, ret, flag = 0, l;

    static struct option long_options[] = {
        {"config",    required_argument, NULL, 'c'},
        {"speed",     required_argument, NULL, 's'},
        {"stopbit",   required_argument, NULL, 'b'},
        {"paritybit", required_argument, NULL, 'p'},
        {0, 0, 0, 0}
    };

    /* May be argv should be argv + 1? */
    while ((opt = getopt_long(argc, argv, "c:s:b:p:", long_options,
                              &l)) != -1) {
        flag = 1; /* So we get some argument */

        switch (opt) {
        case 'c':
            /* If user's file doesn't exist -> exit with error  */
            ret = parse_config_file(uart_termios, optarg);
            if (ret == -1) {
                printf("File: %s doesn't exist", optarg);
                restore_terms_settings_and_die(-1);
            }
            break;
        case 's':
            set_speed(uart_termios, optarg);
            break;

        case 'b':
            set_stopbit(uart_termios, optarg);
            break;

        case 'p':
            set_paritybit(uart_termios, optarg);
            break;
            
        }
    }

    return flag ? 0 : -1;
}

/*
  Firstly of all we parse arguments, if we get -c then parse this file.
  If we have not enough arguments we use settings, that was installed
  on uart previously, or by default. (We we have argument -c with
  other arguments it can be dangerous).

  Secondly, if no arguments are provided at all, we try to open
  config files on standart places:
  ~/.config/.stuaterm.conf
  ~/.stuaterm.conf
*/
int main(int argc, char** argv) {
    struct termios uart_termios;
    int ret;
    
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

    /* Set term settings */
    ret = tcgetattr(uart_fd, &uart_termios_old);
    if (ret == -1) {
        perror("tcgetattr failed on uart");
        restore_terms_settings_and_die(ret);
    }

    uart_termios = uart_termios_old;
    

    ret = parse_args(argc, argv, &uart_termios);
    if (ret == -1) {/* No arguments was provided, so try to open
                      some standart config files */
        ret = parse_config_file(&uart_termios, "~/.config/.stuaterm.conf");
        if (ret == -1) { /* Try another place */
            ret = parse_config_file(&uart_termios, "~/.stuaterm.conf");
            if (ret == -1) {
                printf("No config arguments were provide. stuaterm will \
 works with current settings on uart device. (You can check them with stty)");
            }
        }
    }

    prepare_uart(&uart_termios);

    main_loop();
}
