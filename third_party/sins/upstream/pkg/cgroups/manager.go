//go:build cgroups
package cgroups

import (
	"fmt"
	"os"
	"path/filepath"

	"shim-systemctl/pkg/safeunit"
)

const baseDir = "/sys/fs/cgroup/sins"

type Manager struct{}

func NewManager() *Manager {
	return &Manager{}
}

func (m *Manager) SetupServiceCgroup(name string, limits map[string]string) error {
	if err := safeunit.ValidateServiceName(name); err != nil {
		return err
	}
	serviceDir := filepath.Join(baseDir, name)
	if err := os.MkdirAll(serviceDir, 0755); err != nil {
		return fmt.Errorf("failed to create cgroup dir: %w", err)
	}

	for key, val := range limits {
		filename := ""
		switch key {
		case "MemoryMax":
			filename = "memory.max"
		case "CPUQuota":
			filename = "cpu.max"
			val = formatCPUQuota(val)
		}

		if filename != "" && val != "" {
			if err := os.WriteFile(filepath.Join(serviceDir, filename), []byte(val), 0644); err != nil {
				return fmt.Errorf("failed to write %s: %w", filename, err)
			}
		}
	}

	return nil
}

func formatCPUQuota(quotaStr string) string {
	if quotaStr == "" {
		return ""
	}
	var quota int
	fmt.Sscanf(quotaStr, "%d%%", &quota)
	if quota == 0 {
		return ""
	}
	return fmt.Sprintf("%d 100000", quota*1000)
}
