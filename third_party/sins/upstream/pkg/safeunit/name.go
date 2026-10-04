// Package safeunit validates unit / runit service names and related strings
// to limit path traversal and shell injection in generated scripts and syscalls.
package safeunit

import (
	"fmt"
	"path/filepath"
	"regexp"
	"strings"
	"unicode/utf8"
)

const maxServiceNameLen = 128

// NamePattern is a conservative allowlist: one path segment, typical runit layout.
var namePattern = regexp.MustCompile(`^[a-zA-Z0-9][a-zA-Z0-9_.@-]*$`)

// ValidateServiceName returns an error if s is not safe as a runit service directory name.
func ValidateServiceName(s string) error {
	if s == "" {
		return fmt.Errorf("empty service name")
	}
	if strings.Contains(s, "/") || strings.Contains(s, "..") {
		return fmt.Errorf("invalid service name %q", s)
	}
	if strings.HasPrefix(s, ".") {
		return fmt.Errorf("invalid service name %q", s)
	}
	if utf8.RuneCountInString(s) > maxServiceNameLen {
		return fmt.Errorf("service name too long")
	}
	if filepath.Base(s) != s {
		return fmt.Errorf("invalid service name %q", s)
	}
	if !namePattern.MatchString(s) {
		return fmt.Errorf("service name contains disallowed characters: %q", s)
	}
	return nil
}

// chpstUserPattern allows user, group:user, or numeric ids as used by runit chpst -u.
var chpstUserPattern = regexp.MustCompile(`^[a-zA-Z0-9_.:@-]+$`)

// ValidateChpstUserSpec checks Service User= value before embedding in a shell script.
func ValidateChpstUserSpec(s string) error {
	if s == "" {
		return nil
	}
	if utf8.RuneCountInString(s) > 256 {
		return fmt.Errorf("User= value too long")
	}
	if !chpstUserPattern.MatchString(s) {
		return fmt.Errorf("User= contains disallowed characters: %q", s)
	}
	return nil
}

var envKeyPattern = regexp.MustCompile(`^[a-zA-Z_][a-zA-Z0-9_]*$`)

// ValidateEnvKey rejects keys that could escape the env directory (path traversal).
func ValidateEnvKey(k string) error {
	if k == "" {
		return fmt.Errorf("empty environment key")
	}
	if strings.Contains(k, "/") || strings.Contains(k, "..") || strings.Contains(k, "\x00") {
		return fmt.Errorf("invalid environment key %q", k)
	}
	if !envKeyPattern.MatchString(k) {
		return fmt.Errorf("environment key must be a valid shell identifier: %q", k)
	}
	return nil
}
