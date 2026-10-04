package timers

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"shim-systemctl/pkg/safeunit"
	"shim-systemctl/pkg/units"
	"strings"
	"time"
)

type Timer struct {
	Unit     *units.Unit
	Name     string
	Target   string
	NextRun  time.Time
	Interval time.Duration
}

func NewTimer(path string) (*Timer, error) {
	unit, err := units.Parse(path)
	if err != nil {
		return nil, err
	}

	name := filepath.Base(path)
	target := unit.Get("Timer", "Unit")
	if target == "" {
		target = strings.TrimSuffix(name, ".timer") + ".service"
	}

	t := &Timer{
		Unit:   unit,
		Name:   name,
		Target: target,
	}
	t.UpdateNextRun()
	return t, nil
}

func (t *Timer) UpdateNextRun() {
	now := time.Now()
	calendar := t.Unit.Get("Timer", "OnCalendar")

	if calendar != "" {
		// Use new calendar parser for complex specs
		next, err := ParseCalendar(calendar, now)
		if err != nil {
			// Fallback: treat as simple duration
			t.Interval = parseSimpleDuration(calendar)
			t.NextRun = now.Add(t.Interval)
		} else {
			t.NextRun = next
			// Calculate interval for logging
			t.Interval = t.NextRun.Sub(now)
		}
	} else {
		activeSec := t.Unit.Get("Timer", "OnUnitActiveSec")
		if activeSec != "" {
			t.Interval, _ = time.ParseDuration(strings.ReplaceAll(activeSec, " ", ""))
		}
		if t.Interval == 0 {
			t.Interval = 1 * time.Hour
		}
		t.NextRun = now.Add(t.Interval)
	}

	fmt.Printf("Timer %s scheduled for %v\n", t.Name, t.NextRun)
}

func parseSimpleDuration(val string) time.Duration {
	switch strings.ToLower(val) {
	case "minutely":
		return 1 * time.Minute
	case "hourly":
		return 1 * time.Hour
	case "daily":
		return 24 * time.Hour
	case "weekly":
		return 7 * 24 * time.Hour
	case "monthly":
		return 30 * 24 * time.Hour // Approximate
	}
	d, err := time.ParseDuration(val)
	if err == nil {
		return d
	}
	return 1 * time.Hour // Default
}

func (t *Timer) Trigger() {
	fmt.Printf("[%v] Triggering timer %s -> %s\n", time.Now().Format(time.RFC3339), t.Name, t.Target)

	targetBase := strings.TrimSuffix(t.Target, ".service")
	targetBase = strings.TrimSuffix(targetBase, ".socket")
	targetBase = strings.TrimSuffix(targetBase, ".timer")
	if err := safeunit.ValidateServiceName(targetBase); err != nil {
		fmt.Printf("Error triggering %s: invalid target name: %v\n", t.Target, err)
		return
	}

	systemctlPath := os.Getenv("SYSTEMCTL_PATH")
	if systemctlPath == "" {
		systemctlPath = "systemctl"
	}

	cmd := exec.Command(systemctlPath, "start", t.Target)
	cmd.Env = os.Environ()
	if err := cmd.Run(); err != nil {
		fmt.Printf("Error triggering %s: %v\n", t.Target, err)
	}
}
