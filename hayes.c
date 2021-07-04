/*
 * hayes.c
 *
 * Virtual modem
 */

#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/select.h>
#include <sys/time.h>
#include <setjmp.h>
#include <pty.h>
#include <errno.h>
#include <signal.h>


#define NOTHING

#define VERSION "0.10"


/* std is terminal side, dev is connection side.
 * terminal is -t name
 */
int   std_in   = STDIN_FILENO;
int   std_out  = STDOUT_FILENO;
int   dev      = -1;
char *terminal = NULL;

struct termios oldt;
struct termios newt;

jmp_buf jenv;

int quiet = 0;
int verbose = 0;
int echo = 0; 
int *reg = NULL;
pid_t pid = -1;


/* Give usage information
 */
void usage(void) {
    printf("hayes " VERSION "\n\n");
    fprintf(stderr, "hayes [-t terminal]\n\n");
    fprintf(stderr, "  -t terminal\n");
    exit(1);
}


/* handle options
 */
void options(int argc, char **argv) {
    int ch;
    opterr = 1;
    while ((ch = getopt(argc, argv, "?t:")) != -1)
        switch (ch) {
        case 't':
            terminal = strdup(optarg);
            break;
	case '?':
	default:
	    usage();
	    break;
    }
}


void copy(void) {
    struct timeval tv;
    struct timeval last = { 0, 0 };
    fd_set fds;
    int pcnt, n, gap;
    char c;

    if (dev < 0)
	return;
    gap = 0;
    pcnt = 0;
    gettimeofday(&last, NULL);
    for (;;) {

        /* copy mode. Look at both std_in and dev and
	 * copy to the other (std_in to dev and dev to
	 * std_out).
	 *
	 * Looks for in-band <1sec>+++<1sec> to enter into
	 * command mode.
	 */

        FD_ZERO(&fds);
        FD_SET(std_in, &fds);
        FD_SET(dev, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
	select(dev + 1, &fds, NULL, NULL, &tv);

        /* Get gap time. We get here on each character, or
	 * every 10 milliseconds.
	 */
        gettimeofday(&tv, NULL);
	if ((tv.tv_sec - last.tv_sec) >= 1) {
	    /* If we have have seen +++, enter command mode
             */
	    gap = 1;
	    if (pcnt == 3) {
		longjmp(jenv, 1);
	    }
	last = tv;
        }

	if (FD_ISSET(std_in, &fds)) {
	    last = tv;
	    n = read(std_in, &c, 1);
	    if (n == 1) {
		if ((c == '+') && (pcnt == 0) && gap) {
	            ++pcnt;
		} else if ((c == '+') && pcnt) {
		    gap = 0;
	            ++pcnt;
		} else if (c != '+') {
		    gap = 0;
		    while (pcnt) {
			c = '+';
			write(dev, &c, 1);
		    }
		    pcnt = 0;
		}
		if (pcnt == 0) 
                    write(dev, &c, 1);
	    }
	} else if (FD_ISSET(dev, &fds)) {
	    n = read(dev, &c, 1);
	    if (n == 1)
                write(std_out, &c, 1);
	    if (n == -1) {
		if (errno == EAGAIN)
		    NOTHING;
		else if (errno == EINTR)
		    NOTHING;
	        else if (errno == EWOULDBLOCK)
		    NOTHING;
	        else {
		    close(dev);
		    dev = -1;
		    longjmp(jenv, 1);
		}	
	    }
	} else {
	    NOTHING;
	}
    }
}


#define OK          0
#define CONNECT     1
#define NO_CARRIER  3
#define ERROR       4
#define NO_DIALTONE 6
#define BUSY        7


void response(int r) {
    char *resp[] = { "OK", "CONNECT", "", "NO CARRIER",
	             "ERROR", "", "NO DIALTONE", "BUSY"
                   };

    if (quiet > 0)
	return;
    if ((r < 0) || (r > 7))
	response(ERROR);
    else {
        if (verbose == 0)
    	    printf("%s\r\n", resp[r]);
	else
	    printf("%d\r\n", r);
    }
}


void help(void) {
    printf("AT\r\n");
    printf("\r\n");
    printf("  ? help\r\n");
    printf("  X exit\r\n");
    printf("\r\n");
    printf("  O to call\r\n");
    printf("  H hook\r\n");
    printf("  D dial\r\n");
    printf("\r\n");
    printf("  E echo\r\n");
    printf("  V verbose\r\n");
    printf("  Q quiet\r\n");
}

void hangup(void) {
    if (dev >= 0)
	close(dev);
    dev = -1;
    if (pid >= 0) {
	kill(pid, 9);
	pid = -1;
    }
}

void dial(char *s) {
    hangup();
    if (toupper(*s) == 'T')
        ++s;
    else if (toupper(*s) == 'P')
        ++s;
    pid = forkpty(&dev, NULL, NULL, NULL);
    if (pid < 0)
	dev = -1;
    if (pid == 0) {
	system(s);
	pid = -1;
	exit(0);
    }
    if (dev >= 0)
	response(CONNECT);
    copy();
}

void command(void) {
    static char buf[256];
    char c;
    char *s;
    int r, n;

    setjmp(jenv);
    r = OK;
    for (;;) {
        response(r);
	if (echo == 0) {
            tcsetattr(std_in, TCSAFLUSH, &oldt);
	    fgets(buf, sizeof buf, stdin);
	    if (buf[strlen(buf) - 1] == '\n')
	        buf[strlen(buf) - 1] = '\0';
            tcsetattr(std_in, TCSAFLUSH, &newt);
	} else {
            s = buf;
	    for (n = 0; n < sizeof(buf) - 1; ++n) {
		read(std_in, &c, 1);
		if (c == '\r')
		    break;
		*s++ = c;
	    }
	    *s = '\0';
	}
	s = buf;
        while (*s && isspace(*s))
	    ++s;
	if (toupper(*s) != 'A') {
	    r = ERROR;
	    continue;
	}
	++s;
	if (toupper(*s) != 'T') {
	    r = ERROR;
	    continue;
	}
	reg = NULL;
	r = OK;
        do {
	    ++s;
	    switch (*s) {
	    case 'x': case 'X':
		hangup();
		return;
	    case '?':
		help();
		break;
	    case 'd': case 'D':
		dial(s + 1);
		longjmp(jenv, 1);
	    case 'h': case 'H':
		hangup();
		longjmp(jenv, 1);
	    case 'o': case 'O':
                copy();
		longjmp(jenv, 1);
            case '0': case '1': case '2': case '3': case '4':
	    case '5': case '6': case '7': case '8': case '9':
		if (reg)
		    *reg = *reg * 10 + *s - '0';
	        break;
	    case 'e': case 'E':
	        reg = &echo;
		*reg = 0;
	        break;
            case 'v': case 'V':
		reg = &verbose;
		*reg = 0;
	        break;
	    case 'q': case 'Q':
		reg = &quiet;
		*reg = 0;
	        break;
            case ' ': case '\0':
	        break;
	    default:
		r = ERROR;
		*s = '\0';
		break;
	    }
	} while (*s);
    }
}


/* Signon giving option feedback
 */
void signon(void) {
    printf("hayes " VERSION "\n\n");
}


/* Hayes main
 */
int main(int argc, char **argv) {

    options(argc, argv);

    signon();
    if (terminal) {
        std_out = open(terminal, O_RDWR);
	if (std_out < 0) {
	    fprintf(stderr, "can\'t open terminal %s\n", terminal);
	    exit(1);
	}
	std_in = std_out;
    }

    tcgetattr(std_in, &oldt);
    newt = oldt;
    cfmakeraw(&newt);
    tcsetattr(std_in, TCSANOW, &newt);

    setvbuf(stdout, NULL, _IONBF, 0);

    command();

    tcsetattr(std_in, TCSAFLUSH, &oldt);

    return 0;
}
