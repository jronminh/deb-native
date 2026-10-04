package systemctl

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"shim-systemctl/pkg/runit"
	"shim-systemctl/pkg/safeunit"
)

func IsSystemRunning() int {
	if _, err := os.Stat("/run/runit/service"); err == nil {
		return 0
	}
	if _, err := os.Stat("/etc/runit/2"); err == nil {
		return 0
	}
	if _, err := os.Stat("/var/service"); err == nil {
		return 0
	}
	fmt.Println("offline")
	return 1
}

func ListUnits(mgr *runit.Manager) error {
	files, err := os.ReadDir(mgr.ServiceDir)
	if err != nil {
		return err
	}
	for _, f := range files {
		if !f.IsDir() || strings.HasPrefix(f.Name(), ".") {
			continue
		}
		name := f.Name()
		if safeunit.ValidateServiceName(name) != nil {
			continue
		}
		load := "loaded"
		active := "inactive"
		sub := "dead"
		cmd := exec.Command("sv", "status", name)
		out, _ := cmd.CombinedOutput()
		line := strings.TrimSpace(string(out))
		if strings.HasPrefix(line, "run:") {
			active = "active"
			sub = "running"
		} else if strings.HasPrefix(line, "finish:") {
			active = "inactive"
			sub = "dead"
		}
		fmt.Printf("%-25s %-40s %-12s %-12s %s\n", name+".service", name+".service", load, active, sub)
	}
	return nil
}

func ListUnitFiles(ctx *Ctx) error {
	seen := make(map[string]bool)
	for _, dir := range ctx.UnitPaths {
		globs := []string{"*.service", "*.socket", "*.timer"}
		for _, g := range globs {
			matches, _ := filepath.Glob(filepath.Join(dir, g))
			for _, p := range matches {
				if seen[p] {
					continue
				}
				seen[p] = true
				rel := filepath.Base(p)
				state := "static"
				if strings.HasPrefix(dir, "/etc/systemd") {
					state = "disabled"
				}
				svcKey := strings.TrimSuffix(rel, ".service")
				svcKey = strings.TrimSuffix(svcKey, ".socket")
				svcKey = strings.TrimSuffix(svcKey, ".timer")
				enable := filepath.Join(ctx.Mgr.EnableDir, svcKey)
				if st, err := os.Lstat(enable); err == nil && !st.IsDir() {
					state = "enabled"
				}
				fmt.Printf("%-45s %s\n", rel, state)
			}
		}
	}
	return nil
}
