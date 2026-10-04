package units

import (
	"strings"
)

// ShellEscapeSingleQuoted returns s wrapped for POSIX sh single-quoted context,
// so `exec sh -c ` + ShellEscapeSingleQuoted(line) is safe for arbitrary systemd ExecStart text.
func ShellEscapeSingleQuoted(s string) string {
	return "'" + strings.ReplaceAll(s, "'", `'"'"'`) + "'"
}
