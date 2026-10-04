package main

import (
	"fmt"
	"os"
	"path/filepath"
	"shim-systemctl/pkg/timers"
	"strings"
	"time"
)

func main() {
	unitPath := os.Getenv("SYSTEMD_UNIT_PATH")
	if unitPath == "" {
		unitPath = "/etc/systemd/system"
	}

	activeTimers := make(map[string]*timers.Timer)

	fmt.Println("SINS Timer Daemon started")

	for {
		files, err := os.ReadDir(unitPath)
		if err == nil {
			for _, f := range files {
				if strings.HasSuffix(f.Name(), ".timer") {
					if _, exists := activeTimers[f.Name()]; !exists {
						t, err := timers.NewTimer(filepath.Join(unitPath, f.Name()))
						if err == nil {
							activeTimers[f.Name()] = t
						}
					}
				}
			}
		}

		now := time.Now()
		for _, t := range activeTimers {
			if now.After(t.NextRun) {
				t.Trigger()
				t.UpdateNextRun()
			}
		}

		time.Sleep(10 * time.Second)
	}
}
