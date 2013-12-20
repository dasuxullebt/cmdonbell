#ifdef COMPILE

cc -o cmdonbell cmdonbell.c -lutil

#elifdef COMPILE_DEBUG

cc -g -o cmdonbell cmdonbell.c -lutil

#else

#include <assert.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <spawn.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <stdarg.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>

/* TODO: Use sigaction */
#include <signal.h>
#include <bsd/libutil.h>

static int mpt; /* master pty */
static pid_t pid = 0; /* child pid */
static bool childexit = false;
//struct termios my_termios;

static struct termios   save_termios;
static int              term_saved;


/* FROM http://www.lafn.org/~dave/linux/terminalIO.html - not my own work! */
int tty_raw(int fd) {       /* RAW! mode */
    struct termios  buf;

    if (tcgetattr(fd, &save_termios) < 0) /* get the original state */
        return -1;

    buf = save_termios;

    buf.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
                    /* echo off, canonical mode off, extended input
                       processing off, signal chars off */

    buf.c_iflag &= ~(BRKINT | ICRNL | ISTRIP | IXON);
                    /* no SIGINT on BREAK, CR-toNL off, input parity
                       check off, don't strip the 8th bit on input,
                       ouput flow control off */

    buf.c_cflag &= ~(CSIZE | PARENB);
                    /* clear size bits, parity checking off */

    buf.c_cflag |= CS8;
                    /* set 8 bits/char */

    buf.c_oflag &= ~(OPOST);
                    /* output processing off */

    buf.c_cc[VMIN] = 1;  /* 1 byte at a time */
    buf.c_cc[VTIME] = 0; /* no timer on input */

    if (tcsetattr(fd, TCSAFLUSH, &buf) < 0)
        return -1;

    term_saved = 1;

    return 0;
}


int tty_reset(int fd) { /* set it to normal! */
    if (term_saved)
        if (tcsetattr(fd, TCSAFLUSH, &save_termios) < 0)
            return -1;

    return 0;
}

/* That's my own work (and it probably sucks): */

static int max (int ini, ...) {
  /* the maximum of positive integers, terminated with -1 */
  va_list ap;
  va_start(ap, ini);
  int ret = -1;
  while (ini != -1) {
    ret = (ret < ini) ? ini : ret;
    ini = va_arg(ap, int);
  }
  va_end(ap);
  return ret;
}

/* atexit */

static void close_child (void) {
  if (! childexit) {
    kill (pid, SIGTERM);
  }
}

static void reset_term (void) {
  tty_reset(STDIN_FILENO);
}

/* signal handlers */
static void sigwinch (int sig) {
  (void) sig;
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1) {
    ioctl(mpt, TIOCSWINSZ, &w);
  }
}

static void sigchld (int sig) {
  (void) sig;
  childexit = true;
  _exit(EXIT_SUCCESS);
}

static void sigusr1 (int sig) {
  (void) sig;
  struct winsize w;
  ioctl(STDIN_FILENO, TIOCGWINSZ, &w);
  w.ws_row = 25;
  w.ws_col = 25;
}

int main (int argc, char** argv, char** env) {

  atexit(close_child);
  atexit(reset_term);

  char* shell = getenv("SHELL");
  char* command = NULL;
  char* beepcommand = NULL;
  int opt;

  while ((opt = getopt(argc, argv, "s:c:b:")) != -1) {
    switch(opt) {
    case 's':
      shell = optarg;
      break;
    case 'b':
      beepcommand = optarg;
      break;
    case 'c':
      command = optarg;
      break;
    default:
      fprintf(stderr, "Usage: %s [-s shell] [-c command] [-b beepcommand]\n", argv[0]);
      exit(EXIT_FAILURE);
    }
  }

  if (shell == NULL) {
    fprintf(stderr, "Please set the SHELL environment variable or specify -s.\n");
    exit(EXIT_FAILURE);
  }

  if (beepcommand == NULL) {
    fprintf(stderr, "Please provide a beep command via -b");
    exit(EXIT_FAILURE);
  }

  pid = forkpty (&mpt, NULL, NULL, NULL);
  if (pid == -1) {
    perror("Could not fork");
    exit(EXIT_FAILURE);
  } else if (pid == 0) {
    /* We are the child */
    char* spawnedArgsC[] = { shell, "-c", command, NULL };
    char* spawnedArgsNC[] = { shell, NULL };
    int ex;
    if (command) {
      ex = execve(shell, spawnedArgsC, env);
    } else {
      ex = execve(shell, spawnedArgsNC, env);
    }
    if (-1 == ex) {
      perror("Cannot exec");
      exit(EXIT_FAILURE);
    }
  } else {
    /* We are the parent */

    tty_raw(STDIN_FILENO);

    fd_set rfds, wfds;
    int sel, nfds;

    char mystdin[4096], yourstdout[4096];
    int mystdindef = 0, yourstdoutdef = 0;

    signal(SIGWINCH, sigwinch);
    signal(SIGCHLD, sigchld);
    signal(SIGUSR1, sigusr1);

    while (1) {
    begin_select_loop:

      nfds = 0;

      FD_ZERO(&rfds);
      FD_ZERO(&wfds);

      /* read if buffers are empty, write if buffers are filled */

      if (mystdindef == 0) {
	FD_SET(STDIN_FILENO, &rfds);
	nfds = max(nfds, STDIN_FILENO, -1);
      } else {
	FD_SET(mpt, &wfds);
	nfds = max(nfds, mpt, -1);
      }

      if (yourstdoutdef == 0) {
	FD_SET(mpt, &rfds);
	nfds = max(nfds, mpt, -1);
      } else {
	FD_SET(STDOUT_FILENO, &wfds);
	nfds = max(nfds, STDOUT_FILENO, -1);
      }

      sel = select(1 + nfds, &rfds, &wfds, NULL, NULL);
      if (sel < 0) {
	int e = errno;
	if (e == EINTR) {
	  /* A signal has interrupted us. */
	  goto begin_select_loop;
	} else {
	  fprintf(stderr, "Error calling select: %s\n", strerror(e));
	  exit(EXIT_FAILURE);
	}
      }

      if (FD_ISSET(STDIN_FILENO, &rfds)) {
	mystdindef = read (STDIN_FILENO, mystdin, sizeof(mystdin));
	if (mystdindef == -1) {
	  perror("Reading from stdin");
	  exit(EXIT_FAILURE);
	} else if (mystdindef == 0) {
	  /* EOF */
	  exit(EXIT_SUCCESS);
	}
      }

      if (FD_ISSET(mpt, &rfds)) {
	yourstdoutdef = read (mpt, yourstdout, sizeof(mystdin));
	if (yourstdoutdef == -1) {
	  perror("Reading from master");
	  exit(EXIT_FAILURE);
	} else if (yourstdoutdef == 0) {
	  /* EOF */
	  exit(EXIT_SUCCESS);
	}
      }

      if (FD_ISSET(STDOUT_FILENO, &wfds)) {
	int written = write(STDOUT_FILENO, yourstdout, yourstdoutdef);
	if (written == -1) {
	  perror("Writing to stdout");
	  exit(EXIT_FAILURE);
	} else {
	  int cchar;
	  bool ex = false;
	  for (cchar = 0; cchar < written; ++cchar) {
	    if (yourstdout[cchar] == '\a') ex = true;
	  }
	  if (ex) {
	    /* execute the beepcommand */
	    pid_t npid = fork();
	    if (pid == -1) {
	      perror("Could not fork.");
	      exit(EXIT_FAILURE);
	    } else if (pid == 0) {
	      /* we are again parent. nothing to do. */
	    } else {
	      /* we are the child. execute the command. */
	      char* spawnedArgs[] = {shell, "-c", beepcommand, NULL};
	      if (-1 == execve(shell, spawnedArgs, env)) {
		perror("Cannot exec");
		exit(EXIT_FAILURE);
	      }
	    }
	  }
	  if (written < yourstdoutdef) {
	    memmove(yourstdout, yourstdout + written, yourstdoutdef -= written );
	  } else {
	    yourstdoutdef = 0;
	  }
	}
      }

      if (FD_ISSET(mpt, &wfds)) {
	int written = write(mpt, mystdin, mystdindef);
	if (written == -1) {
	  perror("Writing to stdout");
	  exit(EXIT_FAILURE);
	} else if (written < mystdindef) {
	  memmove(mystdin, mystdin + written, mystdindef -= written );
	} else {
	  mystdindef = 0;
	}
      }
    }
  }
}

#endif
