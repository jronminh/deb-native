/* Hand-run check for dn_policy_translate_path()/detranslate_path(),
 * not wired into any build yet (leading dot, like tracer/.check_*.c).
 * Needs a real, existing directory (not just a string) since
 * dn_policy_init() now also sets up fake-root/hardlink state under it
 * (mkdir()s that fail against a fictional path) -- see .check_hardlink.c
 * for why it's rooted under home, not /tmp. */
#include "dn-policy.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>

static int n_fail = 0;

static void expect_translate(const char *guest, const char *want, int want_status)
{
	char out[PATH_MAX];
	int status = dn_policy_translate_path(guest, out, sizeof(out));

	if (status != want_status) {
		printf("FAIL translate(%s): status %d, want %d\n", guest, status, want_status);
		n_fail++;
		return;
	}
	if (status == 0 && strcmp(out, want) != 0) {
		printf("FAIL translate(%s): got %s, want %s\n", guest, out, want);
		n_fail++;
		return;
	}
	printf("ok   translate(%s) -> %s\n", guest, status == 0 ? out : "(error)");
}

static void expect_detranslate(const char *host, const char *want, int want_status)
{
	char out[PATH_MAX];
	int status = dn_policy_detranslate_path(host, out, sizeof(out));

	if (status != want_status) {
		printf("FAIL detranslate(%s): status %d, want %d\n", host, status, want_status);
		n_fail++;
		return;
	}
	if (status == 0 && strcmp(out, want) != 0) {
		printf("FAIL detranslate(%s): got %s, want %s\n", host, out, want);
		n_fail++;
		return;
	}
	printf("ok   detranslate(%s) -> %s\n", host, status == 0 ? out : "(error)");
}

int main(void)
{
	char tree[] = "/data/data/com.termux/files/home/.dnp-check-translate.XXXXXX";
	char rt[PATH_MAX];
	char want[PATH_MAX];
	int status;

	setbuf(stdout, NULL);

	/* Before init: everything rejected.  */
	status = dn_policy_translate_path("/etc/passwd", (char[PATH_MAX]){0}, PATH_MAX);
	assert(status == -EINVAL);

	if (mkdtemp(tree) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(rt, sizeof(rt), "%s/.rt", tree);
	status = mkdir(rt, 0700);
	assert(status == 0);

	status = dn_policy_init(tree, rt);
	assert(status == 0);

	/* Ordinary guest path -> prefixed.  */
	snprintf(want, sizeof(want), "%s/etc/passwd", tree);
	expect_translate("/etc/passwd", want, 0);
	expect_translate("/", tree, 0);

	/* Passthrough, unchanged.  */
	expect_translate("/proc/self/exe", "/proc/self/exe", 0);
	expect_translate("/sys/class", "/sys/class", 0);
	expect_translate("/dev/null", "/dev/null", 0);
	/* Not a real prefix match ("/devx" is not under "/dev").  */
	snprintf(want, sizeof(want), "%s/devx/foo", tree);
	expect_translate("/devx/foo", want, 0);

	/* No double translation: already a host path.  */
	snprintf(want, sizeof(want), "%s/etc/passwd", tree);
	expect_translate(want, want, 0);
	snprintf(want, sizeof(want), "%s/.rt/lib/libc.so.6", tree);
	expect_translate(want, want, 0);

	/* Relative path rejected.  */
	status = dn_policy_translate_path("etc/passwd", (char[PATH_MAX]){0}, PATH_MAX);
	assert(status == -EINVAL);

	/* Reverse translation.  */
	{
		char host[PATH_MAX];
		snprintf(host, sizeof(host), "%s/etc/passwd", tree);
		expect_detranslate(host, "/etc/passwd", 0);
	}
	expect_detranslate(tree, "/", 0);
	expect_detranslate("/proc/self/cwd", "/proc/self/cwd", 0);
	status = dn_policy_detranslate_path("/data/other/etc/passwd", (char[PATH_MAX]){0}, PATH_MAX);
	assert(status == -ENOENT);

	/* RT must sit inside TREE.  */
	status = dn_policy_init(tree, "/data/elsewhere");
	assert(status == -EINVAL);

	if (n_fail == 0)
		printf("all checks passed\n");
	else
		printf("%d check(s) FAILED\n", n_fail);

	return n_fail == 0 ? 0 : 1;
}
