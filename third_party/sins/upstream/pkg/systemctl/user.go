package systemctl

import (
	"fmt"
	"os"
	"strings"
)

// UserModeBanner is printed once when --user is used (stderr) so AUR/desktop users know the setup.
func UserModeBanner() string {
	var b strings.Builder
	b.WriteString("SINS (--user): user services are enabled under ~/.runit/service\n")
	b.WriteString("Unit files: ~/.config/systemd/user, ~/.local/share/systemd/user\n")
	b.WriteString("Tip: Ensure runsvdir is monitoring ~/.runit/service for user services to run.\n")
	return b.String()
}

// UserModeBlocksMutation reports commands that would misleadingly touch system runit while --user was passed.
func UserModeBlocksMutation(cmd string) bool {
	switch cmd {
	case "start", "stop", "restart", "try-restart", "reload", "reload-or-restart",
		"try-reload-or-restart", "enable", "disable", "mask", "unmask",
		"kill", "preset":
		return true
	default:
		return false
	}
}

// PrintUserMutationError explains exit to stderr.
func PrintUserMutationError(cmd string) {
	fmt.Fprintf(os.Stderr, "SINS: command %q is not supported with --user (no per-user runit generation). Omit --user or use a system-wide service.\n", cmd)
}
