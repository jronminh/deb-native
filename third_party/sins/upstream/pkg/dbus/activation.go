//go:build dbus

package dbus

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"

	"github.com/godbus/dbus/v5"
)

// DbusService represents a D-Bus service file (usually in /usr/share/dbus-1/services/)
type DbusService struct {
	Name string // The bus name (e.g., org.gnome.SettingsDaemon)
	Exec string // Command to execute
	Path string // Path to the .service file
}

// ActivationHandler watches for D-Bus service activation requests and starts services on-demand.
type ActivationHandler struct {
	services      map[string]*DbusService // name -> service
	mu            sync.RWMutex
	systemctlPath string
}

// NewActivationHandler creates a handler that monitors D-Bus service directories.
func NewActivationHandler(systemctlPath string) *ActivationHandler {
	if systemctlPath == "" {
		systemctlPath = "systemctl"
	}
	return &ActivationHandler{
		services:      make(map[string]*DbusService),
		systemctlPath: systemctlPath,
	}
}

// LoadServiceFiles scans standard D-Bus service directories and loads .service files.
func (ah *ActivationHandler) LoadServiceFiles() error {
	dirs := []string{
		"/usr/share/dbus-1/services",
		"/usr/local/share/dbus-1/services",
	}

	// Add user directory if in session mode
	if home := os.Getenv("HOME"); home != "" {
		dirs = append([]string{filepath.Join(home, ".local/share/dbus-1/services")}, dirs...)
	}

	for _, dir := range dirs {
		entries, err := os.ReadDir(dir)
		if err != nil {
			continue
		}
		for _, entry := range entries {
			if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".service") {
				continue
			}
			path := filepath.Join(dir, entry.Name())
			svc, err := parseDbusServiceFile(path)
			if err != nil {
				fmt.Printf("Warning: failed to parse %s: %v\n", path, err)
				continue
			}
			ah.mu.Lock()
			ah.services[svc.Name] = svc
			ah.mu.Unlock()
		}
	}
	return nil
}

// parseDbusServiceFile parses a D-Bus .service file (simple INI-like format).
func parseDbusServiceFile(path string) (*DbusService, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	svc := &DbusService{Path: path}
	lines := strings.Split(string(data), "\n")

	inDbusService := false
	for _, line := range lines {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		if line == "[D-BUS Service]" {
			inDbusService = true
			continue
		}
		if strings.HasPrefix(line, "[") {
			inDbusService = false
			continue
		}

		if inDbusService {
			if idx := strings.Index(line, "="); idx > 0 {
				key := strings.TrimSpace(line[:idx])
				val := strings.TrimSpace(line[idx+1:])
				switch key {
				case "Name":
					svc.Name = val
				case "Exec":
					svc.Exec = val
				}
			}
		}
	}

	if svc.Name == "" {
		return nil, fmt.Errorf("no Name field in %s", path)
	}
	return svc, nil
}

// MaybeActivate checks if a name is a known D-Bus service and starts it.
// Returns true if activation was attempted.
func (ah *ActivationHandler) MaybeActivate(name string) bool {
	ah.mu.RLock()
	svc, ok := ah.services[name]
	ah.mu.RUnlock()

	if !ok {
		return false
	}

	fmt.Printf("D-Bus activation: starting %s (%s)\n", name, svc.Exec)

	// Parse the Exec command
	parts := strings.Fields(svc.Exec)
	if len(parts) == 0 {
		fmt.Printf("D-Bus activation: empty Exec for %s\n", name)
		return true
	}

	// Start the service
	cmd := exec.Command(parts[0], parts[1:]...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Env = os.Environ()

	// Set environment for D-Bus session if available
	if dbusAddr := os.Getenv("DBUS_SESSION_BUS_ADDRESS"); dbusAddr != "" {
		cmd.Env = append(cmd.Env, "DBUS_SESSION_BUS_ADDRESS="+dbusAddr)
	}

	if err := cmd.Start(); err != nil {
		fmt.Printf("D-Bus activation: failed to start %s: %v\n", name, err)
		return true
	}

	// Detach - we don't wait for it
	go func() {
		if err := cmd.Wait(); err != nil {
			fmt.Printf("D-Bus activation: %s exited: %v\n", name, err)
		}
	}()

	return true
}

// ListServices returns all known D-Bus services.
func (ah *ActivationHandler) ListServices() []string {
	ah.mu.RLock()
	defer ah.mu.RUnlock()

	var names []string
	for name := range ah.services {
		names = append(names, name)
	}
	return names
}

// watchForActivation monitors D-Bus for activation requests.
// In a full implementation, this would watch for StartServiceByName method calls.
// For now, it sets up a placeholder that activates on specific signals.
func WatchForActivation(conn *dbus.Conn, ah *ActivationHandler) {
	// D-Bus activation typically works via StartServiceByName method calls
	// which are handled by the bus daemon, not directly by watching signals.
	// This is a placeholder for future full implementation.
	fmt.Println("D-Bus activation watcher started (placeholder)")
}
