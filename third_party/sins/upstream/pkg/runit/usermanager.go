package runit

import (
	"os"
	"path/filepath"
)

// UserManager is a per-user runit manager (separate tree in ~/.runit).
type UserManager struct {
	*Manager
}

// NewUserManager creates a manager for user services.
// Uses XDG paths:
//   - SV dir: ~/.runit/sv (service definitions)
//   - Enable dir: ~/.runit/service (symlinks, like /var/service)
//   - Journal: ~/.local/share/sins/journal.sins
func NewUserManager() *UserManager {
	home := os.Getenv("HOME")
	if home == "" {
		// Fallback, shouldn't happen on normal systems
		home = "."
	}

	svDir := os.Getenv("RUNIT_USER_SV_DIR")
	if svDir == "" {
		svDir = filepath.Join(home, ".runit", "sv")
	}

	enableDir := os.Getenv("RUNIT_USER_SERVICE_DIR")
	if enableDir == "" {
		enableDir = filepath.Join(home, ".runit", "service")
	}

	// Create directories if they don't exist
	_ = os.MkdirAll(svDir, 0755)
	_ = os.MkdirAll(enableDir, 0755)

	return &UserManager{
		Manager: &Manager{
			ServiceDir:    svDir,
			EnableDir:     enableDir,
			CgroupManager: nil, // User services don't get cgroups (no permissions)
		},
	}
}

// IsUserServiceAvailable checks if user runit is set up and supervisable.
func (um *UserManager) IsUserServiceAvailable() bool {
	// Check if we can create the supervise directory structure
	// (runsv must be monitoring ~/.runit/service)
	supervisePath := filepath.Join(um.EnableDir, "supervise")
	_, err := os.Stat(supervisePath)
	return err == nil
}
