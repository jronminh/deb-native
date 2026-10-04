// Package targets provides handling for systemd .target units.
// Targets are grouping units that aggregate other units via Wants=/Requires=.
package targets

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"shim-systemctl/pkg/safeunit"
	"shim-systemctl/pkg/units"
	"strings"
)

// TargetResolver handles resolving target dependencies and operations.
type TargetResolver struct {
	UnitPaths []string
}

// NewTargetResolver creates a resolver with the given unit search paths.
func NewTargetResolver(unitPaths []string) *TargetResolver {
	return &TargetResolver{UnitPaths: unitPaths}
}

// FindTarget finds a .target unit file.
func (tr *TargetResolver) FindTarget(name string) string {
	if !strings.HasSuffix(name, ".target") {
		name += ".target"
	}
	for _, p := range tr.UnitPaths {
		path := filepath.Join(p, name)
		if _, err := os.Stat(path); err == nil {
			return path
		}
	}
	return ""
}

// TargetUnits returns the list of services wanted/required by a target.
// It parses Wants= and Requires= from the target unit.
func (tr *TargetResolver) TargetUnits(targetName string) ([]string, error) {
	path := tr.FindTarget(targetName)
	if path == "" {
		return nil, fmt.Errorf("target %s not found", targetName)
	}

	unit, err := units.Parse(path)
	if err != nil {
		return nil, fmt.Errorf("failed to parse target %s: %w", targetName, err)
	}

	var services []string

	// Parse Wants= (soft dependency)
	wants := unit.GetAll("Unit", "Wants")
	for _, line := range wants {
		for _, dep := range strings.Fields(line) {
			if strings.HasSuffix(dep, ".service") ||
				strings.HasSuffix(dep, ".socket") ||
				strings.HasSuffix(dep, ".timer") {
				services = append(services, dep)
			}
		}
	}

	// Parse Requires= (hard dependency)
	requires := unit.GetAll("Unit", "Requires")
	for _, line := range requires {
		for _, dep := range strings.Fields(line) {
			if strings.HasSuffix(dep, ".service") ||
				strings.HasSuffix(dep, ".socket") ||
				strings.HasSuffix(dep, ".timer") {
				services = append(services, dep)
			}
		}
	}

	// Also check target.wants/ directory
	targetBase := strings.TrimSuffix(filepath.Base(path), ".target")
	for _, p := range tr.UnitPaths {
		wantsDir := filepath.Join(p, targetBase+".target.wants")
		entries, err := os.ReadDir(wantsDir)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if !e.IsDir() {
				services = append(services, e.Name())
			}
		}
	}

	return uniqueStrings(services), nil
}

// IsTarget checks if a unit name is a target.
func IsTarget(name string) bool {
	return strings.HasSuffix(name, ".target")
}

// StartTarget starts/enables all services associated with a target.
// This is a best-effort operation; individual service failures are logged but don't stop others.
func (tr *TargetResolver) StartTarget(targetName string, systemctlPath string) error {
	services, err := tr.TargetUnits(targetName)
	if err != nil {
		return err
	}

	fmt.Printf("Target %s wants %d service(s)\n", targetName, len(services))

	var failed []string
	for _, svc := range services {
		svcBase := strings.TrimSuffix(svc, ".service")
		svcBase = strings.TrimSuffix(svcBase, ".socket")
		svcBase = strings.TrimSuffix(svcBase, ".timer")

		if err := safeunit.ValidateServiceName(svcBase); err != nil {
			fmt.Printf("Skipping invalid service name %q: %v\n", svc, err)
			continue
		}

		fmt.Printf("  Starting %s...\n", svc)
		cmd := exec.Command(systemctlPath, "start", svc)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		if err := cmd.Run(); err != nil {
			fmt.Printf("  Failed to start %s: %v\n", svc, err)
			failed = append(failed, svc)
		}
	}

	if len(failed) > 0 {
		return fmt.Errorf("target %s: %d service(s) failed to start: %v", targetName, len(failed), failed)
	}
	return nil
}

// ListTargets returns all available target units in the search paths.
func (tr *TargetResolver) ListTargets() ([]string, error) {
	var targets []string
	seen := make(map[string]bool)

	for _, p := range tr.UnitPaths {
		entries, err := os.ReadDir(p)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if e.IsDir() {
				continue
			}
			name := e.Name()
			if strings.HasSuffix(name, ".target") && !seen[name] {
				seen[name] = true
				targets = append(targets, name)
			}
		}
	}

	return targets, nil
}

func uniqueStrings(s []string) []string {
	seen := make(map[string]bool)
	var out []string
	for _, v := range s {
		if !seen[v] {
			seen[v] = true
			out = append(out, v)
		}
	}
	return out
}
