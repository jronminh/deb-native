/* Link: gcc -o journal_smoke journal_smoke.c -L../build -Wl,-rpath,'$ORIGIN/../build' -lsystemd */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct sd_journal sd_journal;

int sd_journal_send(const char *fmt, ...);
int sd_journal_open(sd_journal **ret, int flags);
void sd_journal_close(sd_journal *j);
int sd_journal_seek_head(sd_journal *j);
int sd_journal_next(sd_journal *j);
int sd_journal_get_data(sd_journal *j, const char *field, const void **data, size_t *length);
int sd_journal_get_fd(sd_journal *j);
int sd_journal_wait(sd_journal *j, uint64_t timeout_usec);

int main(void) {
	setenv("SINS_JOURNAL_FILE", "/tmp/sins-journal-smoke.sins", 1);
	remove("/tmp/sins-journal-smoke.sins");

	if (sd_journal_send("MESSAGE=smoke-test", "PRIORITY=6", NULL) != 0) {
		fprintf(stderr, "send failed\n");
		return 1;
	}

	sd_journal *j = NULL;
	if (sd_journal_open(&j, 0) != 0 || !j) {
		fprintf(stderr, "open failed\n");
		return 1;
	}
	if (sd_journal_seek_head(j) != 0) {
		fprintf(stderr, "seek_head failed\n");
		sd_journal_close(j);
		return 1;
	}
	if (sd_journal_next(j) != 1) {
		fprintf(stderr, "next failed\n");
		sd_journal_close(j);
		return 1;
	}
	const void *d = NULL;
	size_t len = 0;
	if (sd_journal_get_data(j, "MESSAGE", &d, &len) != 0 || !d) {
		fprintf(stderr, "get_data MESSAGE failed\n");
		sd_journal_close(j);
		return 1;
	}
	/* Exercise notify fd path when inotify is available (Linux). */
	if (sd_journal_get_fd(j) >= 0)
		(void)sd_journal_wait(j, 0);
	printf("%.*s\n", (int)len, (const char *)d);
	sd_journal_close(j);
	return 0;
}
