/*
 * consolehub - one shell, three windows.
 *
 * The bring-up image has to be usable from the serial port, over telnet and
 * on the panel at the same time.  A tty can only have one controlling
 * session, so instead of starting a shell per console we start exactly one
 * shell on a pty and relay bytes:
 *
 *   pty output  ->  /dev/tty0 (fbcon), /dev/ttyS0, every telnet client
 *   serial in   ->  pty
 *   tty0 in     ->  pty  (keyboard, none yet)
 *   telnet in   ->  pty  (with the IAC noise filtered out)
 *
 * The shell itself keeps line editing, history and job control because the
 * pty slave still has a normal line discipline; every console simply sees
 * the echo the kernel produces.
 *
 * Build: static aarch64 binary, installed as /bin/consolehub.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define TELNET_PORT 23

static int clients[MAX_CLIENTS];
static int nclients;
static int ptm = -1;
static int tty0_fd = -1;
static int ttyS0_fd = -1;

/*
 * tcsetattr() reprograms the port, including the divisor.  Leaving the speed
 * at the terminal default (38400) silently killed the 921600 console link, so
 * the serial port has to be set back to the rate the kernel console uses.
 */
static void set_raw(int fd, int is_serial)
{
	struct termios t;

	if (fd < 0 || tcgetattr(fd, &t) != 0)
		return;
	cfmakeraw(&t);
	if (is_serial) {
		cfsetispeed(&t, B921600);
		cfsetospeed(&t, B921600);
		t.c_cflag |= CLOCAL | CREAD;
	}
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(fd, TCSANOW, &t);
}

static void write_all(int fd, const char *buf, int len)
{
	int off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				return;
			return;
		}
		off += n;
	}
}

/*
 * Serial is input-only now: the panel and the telnet clients are the two
 * windows that mirror the session.  Kernel printk still reaches ttyS0
 * because it is registered as a console, so boot logs stay visible there.
 */
static void broadcast(const char *buf, int len)
{
	int i;

	write_all(tty0_fd, buf, len);
	for (i = 0; i < nclients; i++)
		write_all(clients[i], buf, len);
}

static void client_drop(int idx)
{
	close(clients[idx]);
	clients[idx] = clients[nclients - 1];
	nclients--;
}

/*
 * Telnet hands us IAC sequences before the real keystrokes.  We refuse every
 * option (the shell only needs a raw byte stream) and drop the negotiation
 * bytes, otherwise the shell would receive 0xff 0xfb 0x01 as input.
 */
static int telnet_filter(const unsigned char *in, int len, unsigned char *out,
			 int outsz, int fd)
{
	int i = 0, o = 0;
	static const unsigned char refuse[3] = { 255, 252, 0 };	/* IAC WONT */
	unsigned char reply[3];

	while (i < len && o < outsz) {
		if (in[i] != 255) {
			out[o++] = in[i++];
			continue;
		}
		if (i + 1 >= len)
			break;
		switch (in[i + 1]) {
		case 251:	/* WILL  -> DONT */
		case 252:	/* WONT  -> DONT */
			if (i + 2 >= len)
				goto done;
			reply[0] = 255;
			reply[1] = 254;
			reply[2] = in[i + 2];
			write_all(fd, (char *)reply, 3);
			i += 3;
			break;
		case 253:	/* DO */
		case 254:	/* DONT */
			if (i + 2 >= len)
				goto done;
			/* ECHO (1) and SUPPRESS-GO-AHEAD (3) were already
			 * answered in the greeting; a WONT here would undo it. */
			if (in[i + 2] != 1 && in[i + 2] != 3) {
				memcpy(reply, refuse, 2);
				reply[2] = in[i + 2];
				write_all(fd, (char *)reply, 3);
			}
			i += 3;
			break;
		case 250:	/* SB ... IAC SE */
			i += 2;
			while (i + 1 < len && !(in[i] == 255 && in[i + 1] == 240))
				i++;
			i += 2;
			break;
		default:	/* IAC IAC or two-byte command */
			i += 2;
			break;
		}
	}
done:
	return o;
}

static void spawn_shell(void)
{
	char *slave = ptsname(ptm);
	pid_t pid;

	if (!slave)
		return;

	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		int fd;

		setsid();
		fd = open(slave, O_RDWR);
		if (fd < 0)
			_exit(1);
		ioctl(fd, TIOCSCTTY, 1);
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
		if (ptm >= 0)
			close(ptm);
		setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
		setenv("HOME", "/", 1);
		setenv("TERM", "vt100", 1);
		execl("/bin/busybox", "busybox", "sh", "-i", NULL);
		_exit(1);
	}
}

static int listen_socket(void)
{
	struct sockaddr_in sa;
	int one = 1;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(TELNET_PORT);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    listen(fd, 4) != 0) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

int main(void)
{
	int lfd, i;
	unsigned char buf[512], clean[512];

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);

	ptm = posix_openpt(O_RDWR | O_NOCTTY);
	if (ptm < 0 || grantpt(ptm) != 0 || unlockpt(ptm) != 0) {
		printf("consolehub: cannot create pty: %s\n", strerror(errno));
		return 1;
	}
	fcntl(ptm, F_SETFL, O_NONBLOCK);

	tty0_fd = open("/dev/tty0", O_RDWR | O_NONBLOCK);
	ttyS0_fd = open("/dev/ttyS0", O_RDWR | O_NONBLOCK);
	set_raw(ttyS0_fd, 1);
	set_raw(tty0_fd, 0);

	lfd = listen_socket();

	spawn_shell();

	printf("consolehub: shell on pty, mirrored to tty0/ttyS0%s\n",
	       lfd >= 0 ? " and telnet:23" : " (telnet bind failed)");
	printf("consolehub: fds ptm=%d tty0=%d ttyS0=%d listen=%d\n",
	       ptm, tty0_fd, ttyS0_fd, lfd);

	for (;;) {
		struct pollfd pfd[3 + MAX_CLIENTS];
		int n = 0, ret;
		int pty_idx = -1, tty0_idx = -1, ttyS0_idx = -1, lfd_idx = -1;
		int cli_idx[MAX_CLIENTS];

		memset(cli_idx, -1, sizeof(cli_idx));

		pfd[n].fd = ptm;
		pfd[n].events = POLLIN;
		pty_idx = n++;

		if (ttyS0_fd >= 0) {
			pfd[n].fd = ttyS0_fd;
			pfd[n].events = POLLIN;
			ttyS0_idx = n++;
		}
		if (tty0_fd >= 0) {
			pfd[n].fd = tty0_fd;
			pfd[n].events = POLLIN;
			tty0_idx = n++;
		}
		if (lfd >= 0) {
			pfd[n].fd = lfd;
			pfd[n].events = POLLIN;
			lfd_idx = n++;
		}
		for (i = 0; i < nclients && n < (int)(sizeof(pfd) / sizeof(pfd[0])); i++) {
			pfd[n].fd = clients[i];
			pfd[n].events = POLLIN;
			cli_idx[i] = n++;
		}

		ret = poll(pfd, n, -1);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		/* shell output -> everyone */
		if (pfd[pty_idx].revents & POLLIN) {
			ssize_t r = read(ptm, buf, sizeof(buf));

			if (r > 0) {
				broadcast((char *)buf, r);
			} else {
				/* the shell exited: start a fresh one */
				broadcast("\r\n[consolehub] shell exited, respawning\r\n", 41);
				close(ptm);
				ptm = posix_openpt(O_RDWR | O_NOCTTY);
				if (ptm >= 0 && grantpt(ptm) == 0 &&
				    unlockpt(ptm) == 0) {
					fcntl(ptm, F_SETFL, O_NONBLOCK);
					spawn_shell();
				}
			}
		}

		/* serial keyboard -> shell */
		if (ttyS0_idx >= 0 && (pfd[ttyS0_idx].revents & POLLIN)) {
			ssize_t r = read(ttyS0_fd, buf, sizeof(buf));

			if (r > 0)
				write_all(ptm, (char *)buf, r);
		}

		/* panel keyboard (if any) -> shell */
		if (tty0_idx >= 0 && (pfd[tty0_idx].revents & POLLIN)) {
			ssize_t r = read(tty0_fd, buf, sizeof(buf));

			if (r > 0)
				write_all(ptm, (char *)buf, r);
		}

		/* new telnet client */
		if (lfd_idx >= 0 && (pfd[lfd_idx].revents & POLLIN)) {
			int cfd = accept(lfd, NULL, NULL);

			if (cfd >= 0) {
				fcntl(cfd, F_SETFL, O_NONBLOCK);
				if (nclients < MAX_CLIENTS) {
					/* IAC WILL ECHO / WILL SGA / DO SGA: the pty
					 * echoes already, so the client must not
					 * echo locally as well. */
					static const unsigned char hello[] = {
						255, 251, 1, 255, 251, 3, 255, 253, 3
					};

					clients[nclients++] = cfd;
					write_all(cfd, (const char *)hello, sizeof(hello));
					/* make the shell redraw its prompt */
					write_all(cfd, "\r\n", 2);
					write_all(ptm, "\n", 1);
				} else {
					close(cfd);
				}
			}
		}

		/* telnet keystrokes -> shell */
		for (i = nclients - 1; i >= 0; i--) {
			int idx = cli_idx[i];
			ssize_t r;

			if (idx < 0 || !(pfd[idx].revents & (POLLIN | POLLHUP)))
				continue;
			r = read(clients[i], buf, sizeof(buf));
			if (r <= 0) {
				client_drop(i);
				continue;
			}
			r = telnet_filter(buf, r, clean, sizeof(clean), clients[i]);
			if (r > 0)
				write_all(ptm, (char *)clean, r);
		}
	}

	return 0;
}
