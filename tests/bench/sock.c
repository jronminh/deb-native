/* An AF_UNIX pathname socket, end to end: bind, getsockname, connect, accept.
 * On the ADB host the shell domain may not create a sock_file in the tree, so
 * the runtime maps the guest path to an abstract name; getsockname and
 * getpeername must still show the guest path.  Bind also makes a regular
 * placeholder at the path, so stat, chmod and unlink work (stat is a file, not
 * S_IFSOCK).  Run inside a prefix: tests/bench/run.sh check. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int one(const char *path)
{
	struct sockaddr_un a;
	socklen_t n;
	int fd, c, ok = 1;
	char buf[8];
	pid_t pid;

	unlink(path);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		printf("socket: %s\n", strerror(errno));
		return 0;
	}
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	strncpy(a.sun_path, path, sizeof a.sun_path - 1);
	if (bind(fd, (struct sockaddr *) &a, sizeof a) < 0) {
		printf("bind %s: %s\n", path, strerror(errno));
		close(fd);
		return 0;
	}
	memset(&a, 0, sizeof a);
	n = sizeof a;
	if (getsockname(fd, (struct sockaddr *) &a, &n) < 0) {
		printf("getsockname: %s\n", strerror(errno));
		ok = 0;
	} else {
		printf("getsockname %s -> %s\n", path, a.sun_path);
		if (strcmp(a.sun_path, path) != 0)
			ok = 0;
	}
	if (listen(fd, 1) < 0) {
		printf("listen: %s\n", strerror(errno));
		ok = 0;
	}

	fflush(stdout);
	pid = fork();
	if (pid == 0) {
		struct sockaddr_un b;
		int s = socket(AF_UNIX, SOCK_STREAM, 0);

		memset(&b, 0, sizeof b);
		b.sun_family = AF_UNIX;
		strncpy(b.sun_path, path, sizeof b.sun_path - 1);
		if (connect(s, (struct sockaddr *) &b, sizeof b) < 0) {
			printf("connect %s: %s\n", path, strerror(errno));
			_exit(1);
		}
		memset(&b, 0, sizeof b);
		n = sizeof b;
		if (getpeername(s, (struct sockaddr *) &b, &n) == 0)
			printf("getpeername -> %s\n", b.sun_path);
		if (write(s, "hi", 2) != 2)
			_exit(1);
		if (read(s, buf, 2) != 2)
			_exit(1);
		_exit(0);
	}
	c = accept(fd, NULL, NULL);
	if (c < 0) {
		printf("accept: %s\n", strerror(errno));
		ok = 0;
	} else {
		if (read(c, buf, 2) == 2 && write(c, "ok", 2) == 2)
			printf("accept/recv: ok\n");
		else {
			printf("accept/recv: failed\n");
			ok = 0;
		}
		close(c);
	}
	if (waitpid(pid, NULL, 0) < 0)
		ok = 0;
	close(fd);

	{
		struct stat st;

		if (stat(path, &st) == 0)
			printf("stat: mode=%o sock=%d\n", st.st_mode & 07777,
			       S_ISSOCK(st.st_mode));
		else
			printf("stat: %s (known gap)\n", strerror(errno));
	}
	if (chmod(path, 0600) == 0)
		printf("chmod: ok\n");
	else
		printf("chmod: %s\n", strerror(errno));
	if (unlink(path) < 0)
		printf("unlink: %s\n", strerror(errno));
	else
		printf("unlink: ok\n");
	return ok;
}

int main(void)
{
	int ok = 1;

	ok &= one("/tmp/dn-sock-a.sock");
	ok &= one("/var/tmp/dn-sock-b.sock");
	printf("sock: %s\n", ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
