package systemctl

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"shim-systemctl/pkg/runit"
	"shim-systemctl/pkg/units"
)

const statusLogLines = 40

// PrintServiceStatus prints a systemd-like status block and returns exit code (0 active, 3 inactive, 4 not found).
func PrintServiceStatus(ctx *Ctx, mgr *runit.Manager, name string, follow bool) int {
	cleanName := strings.TrimSuffix(name, ".service")
	svPath := filepath.Join(mgr.ServiceDir, cleanName)
	unitPath := ctx.FindUnitFile(name)

	desc := ""
	execStart := ""
	if unitPath != "" {
		if u, err := units.Parse(unitPath); err == nil {
			desc = u.Get("Unit", "Description")
			execStart = u.Get("Service", "ExecStart")
		}
	}
	if desc == "" {
		desc = "(no description in unit file)"
	}

	if _, err := os.Stat(svPath); os.IsNotExist(err) {
		fmt.Printf("○ %s - %s\n", name, desc)
		if unitPath != "" {
			fmt.Printf("     Loaded: loaded (%s; static — no generated runit service dir yet)\n", unitPath)
			fmt.Printf("     Active: inactive (dead)\n")
			fmt.Printf("             Unit file exists but %s was not created.\n", filepath.Base(svPath))
			fmt.Println("             Hint: systemctl enable/start (without --user) or daemon-reload after fixing the unit.")
		} else {
			fmt.Println("     not-found")
		}
		return 4
	}

	statPath := filepath.Join(svPath, "supervise", "stat")
	statData := ""
	if b, err := os.ReadFile(statPath); err == nil {
		statData = strings.TrimSpace(string(b))
	}

	cmd := exec.Command("sv", "status", cleanName)
	out, err := cmd.CombinedOutput()
	svLine := strings.TrimSpace(string(out))
	if err != nil {
		svLine = fmt.Sprintf("(sv status failed: %v) %s", err, svLine)
	}

	isRun := strings.HasPrefix(svLine, "run:")
	isDown := strings.HasPrefix(svLine, "down:")
	// One line, systemd-style (avoid "active (running) (running)").
	activeLine := "inactive (dead)"
	if isRun {
		activeLine = "active (running)"
	} else if isDown {
		if strings.Contains(svLine, "want up") || strings.Contains(svLine, "normally up") {
			activeLine = "failed or restarting (runsv)"
		} else {
			activeLine = "inactive (stopped)"
		}
	}

	symbol := "○"
	if isRun {
		symbol = "●"
	}

	fmt.Printf("%s %s - %s\n", symbol, name, desc)
	fmt.Printf("     Loaded: loaded (%s)\n", loadLine(unitPath, svPath))
	fmt.Printf("     Active: %s\n", activeLine)
	if execStart != "" {
		fmt.Printf("      Drive: %s\n", execStart)
	}
	if pid := pidFromSvStatus(svLine); pid != "" {
		fmt.Printf("    Process: pid %s (from runsv/supervise)\n", pid)
	}
	if statData != "" {
		fmt.Printf("  Supervise: state file = %q\n", statData)
	}

	enablePath := filepath.Join(mgr.EnableDir, cleanName)
	if _, err := os.Lstat(enablePath); err == nil {
		fmt.Println("    Enabled: symlink exists under service directory (runit)")
	} else {
		fmt.Println("    Enabled: no symlink (service may run only when started manually)")
	}

	fmt.Printf("    Details: %s\n", svLine)

	logPath := filepath.Join(svPath, "log", "main", "current")
	if fi, err := os.Stat(logPath); err == nil && !fi.IsDir() {
		fmt.Println()
		if follow {
			fmt.Println(" — Log (follow): " + logPath + " —")
			t := exec.Command("tail", "-f", logPath)
			t.Stdout = os.Stdout
			t.Stderr = os.Stderr
			_ = t.Run()
			return 0
		}
		fmt.Println(" — Recent log (" + logPath + ") —")
		printLogTail(logPath, statusLogLines)
	} else {
		fmt.Println("\n     Log: no svlogd file at service/log/main/current — stdout/stderr only in run script context.")
		fmt.Println("           Add a log/ service or check the app's own log paths for errors.")
	}

	if isRun {
		return 0
	}
	if isDown && (strings.Contains(svLine, "normally") || strings.Contains(svLine, "want")) {
		return 3
	}
	return 3
}

func loadLine(unitPath, svPath string) string {
	if unitPath != "" {
		return fmt.Sprintf("%s; runit → %s", unitPath, svPath)
	}
	return fmt.Sprintf("generated tree at %s", svPath)
}

func pidFromSvStatus(line string) string {
	// run: name: (pid 12345) ...
	if i := strings.Index(line, "(pid "); i >= 0 {
		rest := line[i+5:]
		end := strings.IndexAny(rest, ") ")
		if end < 0 {
			end = len(rest)
		}
		if _, err := strconv.Atoi(strings.TrimSpace(rest[:end])); err == nil {
			return strings.TrimSpace(rest[:end])
		}
	}
	return ""
}

func printLogTail(path string, maxLines int) {
	f, err := os.Open(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "     (cannot read log: %v)\n", err)
		return
	}
	defer f.Close()

	var lines []string
	sc := bufio.NewScanner(f)
	const maxScan = 512 * 1024
	buf := make([]byte, 0, 64*1024)
	sc.Buffer(buf, maxScan)
	for sc.Scan() {
		lines = append(lines, sc.Text())
		if len(lines) > maxLines*2 {
			lines = lines[len(lines)-maxLines*2:]
		}
	}
	if len(lines) > maxLines {
		lines = lines[len(lines)-maxLines:]
	}
	for _, ln := range lines {
		fmt.Println("       " + ln)
	}
	if len(lines) == 0 {
		fmt.Println("       (empty)")
	}
}
