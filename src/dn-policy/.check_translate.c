/* Hand-run check for dn_policy_translate_path()/detranslate_path(),
 * not wired into any build yet (leading dot, like tracer/.check_*.c). */
#include "dn-policy.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

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
	int status;

	/* Before init: everything rejected.  */
	status = dn_policy_translate_path("/etc/passwd", (char[PATH_MAX]){0}, PATH_MAX);
	assert(status == -EINVAL);

	status = dn_policy_init("/data/local/deb-native", "/data/local/deb-native/.rt");
	assert(status == 0);

	/* Ordinary guest path -> prefixed.  */
	expect_translate("/etc/passwd", "/data/local/deb-native/etc/passwd", 0);
	expect_translate("/", "/data/local/deb-native", 0);

	/* Passthrough, unchanged.  */
	expect_translate("/proc/self/exe", "/proc/self/exe", 0);
	expect_translate("/sys/class", "/sys/class", 0);
	expect_translate("/dev/null", "/dev/null", 0);
	/* Not a real prefix match ("/devx" is not under "/dev").  */
	expect_translate("/devx/foo", "/data/local/deb-native/devx/foo", 0);

	/* No double translation: already a host path.  */
	expect_translate("/data/local/deb-native/etc/passwd",
			  "/data/local/deb-native/etc/passwd", 0);
	expect_translate("/data/local/deb-native/.rt/lib/libc.so.6",
			  "/data/local/deb-native/.rt/lib/libc.so.6", 0);

	/* Relative path rejected.  */
	status = dn_policy_translate_path("etc/passwd", (char[PATH_MAX]){0}, PATH_MAX);
	assert(status == -EINVAL);

	/* Reverse translation.  */
	expect_detranslate("/data/local/deb-native/etc/passwd", "/etc/passwd", 0);
	expect_detranslate("/data/local/deb-native", "/", 0);
	expect_detranslate("/proc/self/cwd", "/proc/self/cwd", 0);
	status = dn_policy_detranslate_path("/data/other/etc/passwd", (char[PATH_MAX]){0}, PATH_MAX);
	assert(status == -ENOENT);

	/* RT must sit inside TREE.  */
	status = dn_policy_init("/data/local/deb-native", "/data/elsewhere");
	assert(status == -EINVAL);

	if (n_fail == 0)
		printf("all checks passed\n");
	else
		printf("%d check(s) FAILED\n", n_fail);

	return n_fail == 0 ? 0 : 1;
}
