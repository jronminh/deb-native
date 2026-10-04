package main

import (
	"bufio"
	"fmt"
	"os"
	"strings"
	"time"
)

const (
	sinsRecMagic  = 0x4a4e4953 // "SINJ" LE
	sinsRecVer    = 1
	sinsRecHeader = 24 // sizeof(sins_rec_hdr)
)

type sinsRecHdr struct {
	Magic         uint32
	Version       uint32
	RealtimeUsec  uint64
	MonotonicUsec uint64
	NFields       uint32
}

func main() {
	if len(os.Args) > 1 && (os.Args[1] == "-h" || os.Args[1] == "--help") {
		printUsage()
		return
	}

	journalPath := getJournalPath()
	f, err := os.Open(journalPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error opening journal: %v\n", err)
		os.Exit(1)
	}
	defer f.Close()

	// Check for --follow flag
	follow := false
	lines := 0
	unit := ""

	for i := 1; i < len(os.Args); i++ {
		arg := os.Args[i]
		switch arg {
		case "-f", "--follow":
			follow = true
		case "-n", "--lines":
			if i+1 < len(os.Args) {
				fmt.Sscanf(os.Args[i+1], "%d", &lines)
				i++
			}
		case "-u", "--unit":
			if i+1 < len(os.Args) {
				unit = os.Args[i+1]
				i++
			}
		}
	}

	if follow {
		tailFollow(f, unit)
	} else {
		if lines > 0 {
			tailN(f, lines, unit)
		} else {
			dumpAll(f, unit)
		}
	}
}

func getJournalPath() string {
	if p := os.Getenv("SINS_JOURNAL_FILE"); p != "" {
		return p
	}
	if p := os.Getenv("JOURNAL_STREAM"); p != "" {
		return p
	}

	// Try standard locations
	for _, p := range []string{
		"/var/log/sins-journal/journal.sins",
		"/tmp/sins-journal/journal.sins",
	} {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}

	// Default: try to create in /tmp
	return "/tmp/sins-journal/journal.sins"
}

func dumpAll(f *os.File, filterUnit string) {
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := scanner.Text()
		if matchesFilter(line, filterUnit) {
			fmt.Println(formatLine(line))
		}
	}
}

func tailN(f *os.File, n int, filterUnit string) {
	var lines []string
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := scanner.Text()
		if matchesFilter(line, filterUnit) {
			lines = append(lines, line)
			if len(lines) > n {
				lines = lines[1:]
			}
		}
	}
	for _, line := range lines {
		fmt.Println(formatLine(line))
	}
}

func tailFollow(f *os.File, filterUnit string) {
	// Seek to end initially
	f.Seek(0, 2)

	for {
		scanner := bufio.NewScanner(f)
		for scanner.Scan() {
			line := scanner.Text()
			if matchesFilter(line, filterUnit) {
				fmt.Println(formatLine(line))
			}
		}
		time.Sleep(100 * time.Millisecond)
	}
}

func matchesFilter(line, unit string) bool {
	if unit == "" {
		return true
	}
	return strings.Contains(line, "UNIT="+unit) || strings.Contains(line, "_SYSTEMD_UNIT="+unit)
}

func formatLine(line string) string {
	// Parse fields from journal line
	fields := parseFields(line)

	timestamp := fields["__REALTIME_TIMESTAMP"]
	if ts, err := parseTimestamp(timestamp); err == nil {
		timestamp = ts.Format("Jan 02 15:04:05")
	}

	hostname := fields["_HOSTNAME"]
	if hostname == "" {
		hostname, _ = os.Hostname()
	}

	unit := fields["_SYSTEMD_UNIT"]
	if unit == "" {
		unit = fields["UNIT"]
	}
	if unit == "" {
		unit = "unknown"
	}

	message := fields["MESSAGE"]
	if message == "" {
		message = line
	}

	return fmt.Sprintf("%s %s %s: %s", timestamp, hostname, unit, message)
}

func parseFields(line string) map[string]string {
	fields := make(map[string]string)
	for _, part := range strings.Split(line, " ") {
		if idx := strings.Index(part, "="); idx > 0 {
			key := part[:idx]
			val := part[idx+1:]
			fields[key] = val
		}
	}
	return fields
}

func parseTimestamp(ts string) (time.Time, error) {
	// Try parsing as microseconds
	usec, err := parseUint64(ts)
	if err == nil {
		sec := usec / 1000000
		usec %= 1000000
		return time.Unix(int64(sec), int64(usec)*1000), nil
	}

	// Try standard formats
	formats := []string{
		time.RFC3339,
		"2006-01-02 15:04:05",
	}
	for _, f := range formats {
		if t, err := time.Parse(f, ts); err == nil {
			return t, nil
		}
	}

	return time.Time{}, fmt.Errorf("unknown timestamp format")
}

func parseUint64(s string) (uint64, error) {
	var n uint64
	_, err := fmt.Sscanf(s, "%d", &n)
	return n, err
}

func printUsage() {
	fmt.Println("Usage: sins-journalctl [OPTIONS]")
	fmt.Println()
	fmt.Println("Options:")
	fmt.Println("  -f, --follow       Follow journal in real time")
	fmt.Println("  -n, --lines N      Show last N lines")
	fmt.Println("  -u, --unit UNIT    Filter by unit name")
	fmt.Println("  -h, --help         Show this help")
	fmt.Println()
	fmt.Println("Environment:")
	fmt.Println("  SINS_JOURNAL_FILE  Path to journal file")
	fmt.Println()
	fmt.Println("Journal files are searched in order:")
	fmt.Println("  1. $SINS_JOURNAL_FILE")
	fmt.Println("  2. /var/log/sins-journal/journal.sins")
	fmt.Println("  3. /tmp/sins-journal/journal.sins")
}
