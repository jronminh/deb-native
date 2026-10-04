package timers

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// CalendarEvent represents a parsed systemd calendar specification.
// Supports formats like:
//   - minutely, hourly, daily, weekly, monthly, yearly
//   - *:*:0/30 (every 30 seconds)
//   - Mon *-*-01 00:00:00 (first Monday of every month)
//   - Mon..Fri 09:00:00 (weekdays at 9am)
//   - *-05-01 00:00:00 (May 1st every year)
type CalendarEvent struct {
	Raw      string
	Next     time.Time
	Timezone *time.Location
}

// ParseCalendar parses a systemd calendar specification and returns the next occurrence.
func ParseCalendar(spec string, from time.Time) (time.Time, error) {
	spec = strings.TrimSpace(spec)
	if spec == "" {
		return from, fmt.Errorf("empty calendar spec")
	}

	// Normalize lowercase keywords
	switch strings.ToLower(spec) {
	case "minutely":
		return from.Add(time.Minute), nil
	case "hourly":
		return from.Add(time.Hour), nil
	case "daily":
		return nextDaily(from), nil
	case "weekly":
		return nextWeekly(from), nil
	case "monthly":
		return nextMonthly(from), nil
	case "yearly", "annually":
		return nextYearly(from), nil
	}

	// Try parsing as duration (systemd supports "1h", "30min", etc.)
	if d, err := parseSystemdDuration(spec); err == nil {
		return from.Add(d), nil
	}

	// Try parsing full calendar spec
	if t, err := parseFullCalendar(spec, from); err == nil {
		return t, nil
	}

	return from, fmt.Errorf("unsupported calendar spec: %s", spec)
}

// parseSystemdDuration parses systemd time duration strings.
func parseSystemdDuration(s string) (time.Duration, error) {
	// Handle common systemd duration formats
	re := regexp.MustCompile(`(?i)^(\d+)\s*(us|usec|µs|ms|msec|s|sec|seconds?|m|min|minutes?|h|hours?|d|days?|w|weeks?|M|months?|y|years?)?$`)
	matches := re.FindStringSubmatch(s)
	if matches == nil {
		return 0, fmt.Errorf("invalid duration format")
	}

	val, _ := strconv.ParseInt(matches[1], 10, 64)
	unit := strings.ToLower(matches[2])

	switch unit {
	case "us", "usec", "µs":
		return time.Duration(val) * time.Microsecond, nil
	case "ms", "msec":
		return time.Duration(val) * time.Millisecond, nil
	case "s", "sec", "seconds", "":
		return time.Duration(val) * time.Second, nil
	case "m", "min", "minutes":
		return time.Duration(val) * time.Minute, nil
	case "h", "hours":
		return time.Duration(val) * time.Hour, nil
	case "d", "days":
		return time.Duration(val) * 24 * time.Hour, nil
	case "w", "weeks":
		return time.Duration(val) * 7 * 24 * time.Hour, nil
	case "M", "months":
		return time.Duration(val*30) * 24 * time.Hour, nil // Approximate
	case "y", "years":
		return time.Duration(val*365) * 24 * time.Hour, nil // Approximate
	}

	return 0, fmt.Errorf("unknown time unit: %s", unit)
}

// parseFullCalendar parses complex systemd calendar specs like "Mon *-*-01 00:00:00"
func parseFullCalendar(spec string, from time.Time) (time.Time, error) {
	// Simple parser for common patterns
	// Full systemd calendar spec is complex; we support a subset

	// Pattern: Weekday YYYY-MM-DD HH:MM:SS
	// Examples: Mon *-*-01 00:00:00, *-*-* 09:00:00

	parts := strings.Fields(spec)
	if len(parts) < 1 {
		return from, fmt.Errorf("empty spec")
	}

	// Parse weekday restriction if present
	var allowedWeekdays []time.Weekday
	var dateSpec, timeSpec string

	idx := 0
	if isWeekday(parts[idx]) {
		allowedWeekdays = parseWeekdaySpec(parts[idx])
		idx++
	}

	if idx < len(parts) && strings.Contains(parts[idx], "-") {
		dateSpec = parts[idx]
		idx++
	}

	if idx < len(parts) && strings.Contains(parts[idx], ":") {
		timeSpec = parts[idx]
	}

	// Find next matching time
	candidate := from.Add(time.Minute) // Start from next minute
	maxIterations := 366 * 24 * 60 // Max ~1 year of minutes

	for i := 0; i < maxIterations; i++ {
		candidate = candidate.Add(time.Minute)

		// Check date match
		if dateSpec != "" && !matchDateSpec(dateSpec, candidate) {
			continue
		}

		// Check time match
		if timeSpec != "" && !matchTimeSpec(timeSpec, candidate) {
			continue
		}

		// Check weekday match
		if len(allowedWeekdays) > 0 && !containsWeekday(allowedWeekdays, candidate.Weekday()) {
			continue
		}

		return candidate, nil
	}

	return from, fmt.Errorf("could not find next occurrence within 1 year")
}

func isWeekday(s string) bool {
	return strings.Contains(s, "Mon") || strings.Contains(s, "Tue") ||
		strings.Contains(s, "Wed") || strings.Contains(s, "Thu") ||
		strings.Contains(s, "Fri") || strings.Contains(s, "Sat") ||
		strings.Contains(s, "Sun")
}

func parseWeekdaySpec(spec string) []time.Weekday {
	var result []time.Weekday
	spec = strings.ToLower(spec)

	// Handle ranges like Mon..Fri
	if strings.Contains(spec, "..") {
		parts := strings.Split(spec, "..")
		start := weekdayFromString(strings.TrimSpace(parts[0]))
		end := weekdayFromString(strings.TrimSpace(parts[1]))
		for d := start; d <= end; d++ {
			result = append(result, d)
		}
	} else {
		// Single or comma-separated
		for _, part := range strings.Split(spec, ",") {
			d := weekdayFromString(strings.TrimSpace(part))
			if d >= 0 {
				result = append(result, d)
			}
		}
	}

	return result
}

func weekdayFromString(s string) time.Weekday {
	s = strings.ToLower(s)
	switch {
	case strings.HasPrefix(s, "mon"):
		return time.Monday
	case strings.HasPrefix(s, "tue"):
		return time.Tuesday
	case strings.HasPrefix(s, "wed"):
		return time.Wednesday
	case strings.HasPrefix(s, "thu"):
		return time.Thursday
	case strings.HasPrefix(s, "fri"):
		return time.Friday
	case strings.HasPrefix(s, "sat"):
		return time.Saturday
	case strings.HasPrefix(s, "sun"):
		return time.Sunday
	}
	return -1
}

func containsWeekday(list []time.Weekday, d time.Weekday) bool {
	for _, w := range list {
		if w == d {
			return true
		}
	}
	return false
}

func matchDateSpec(spec string, t time.Time) bool {
	// spec format: YYYY-MM-DD or *-*-DD, etc.
	parts := strings.Split(spec, "-")
	if len(parts) != 3 {
		return false
	}

	year, month, day := parts[0], parts[1], parts[2]

	if year != "*" {
		y, _ := strconv.Atoi(year)
		if t.Year() != y {
			return false
		}
	}

	if month != "*" {
		m, _ := strconv.Atoi(month)
		if int(t.Month()) != m {
			return false
		}
	}

	if day != "*" {
		d, _ := strconv.Atoi(day)
		if t.Day() != d {
			return false
		}
	}

	return true
}

func matchTimeSpec(spec string, t time.Time) bool {
	// spec format: HH:MM:SS or HH:MM or *:*:00, etc.
	parts := strings.Split(spec, ":")

	if len(parts) >= 1 && parts[0] != "*" {
		h, _ := strconv.Atoi(parts[0])
		if t.Hour() != h {
			return false
		}
	}

	if len(parts) >= 2 && parts[1] != "*" {
		// Check for step values like */5 or 0/5
		if strings.Contains(parts[1], "/") {
			_, stepStr := splitStep(parts[1])
			step, _ := strconv.Atoi(stepStr)
			if step > 0 && t.Minute()%step != 0 {
				return false
			}
		} else {
			m, _ := strconv.Atoi(parts[1])
			if t.Minute() != m {
				return false
			}
		}
	}

	if len(parts) >= 3 && parts[2] != "*" {
		if strings.Contains(parts[2], "/") {
			_, stepStr := splitStep(parts[2])
			step, _ := strconv.Atoi(stepStr)
			if step > 0 && t.Second()%step != 0 {
				return false
			}
		} else {
			s, _ := strconv.Atoi(parts[2])
			if t.Second() != s {
				return false
			}
		}
	}

	return true
}

func splitStep(s string) (string, string) {
	parts := strings.Split(s, "/")
	if len(parts) == 2 {
		return parts[0], parts[1]
	}
	return s, "1"
}

// Helper functions for named intervals
func nextDaily(from time.Time) time.Time {
	t := from.Add(time.Hour)
	t = time.Date(t.Year(), t.Month(), t.Day(), 0, 0, 0, 0, t.Location())
	if !t.After(from) {
		t = t.Add(24 * time.Hour)
	}
	return t
}

func nextWeekly(from time.Time) time.Time {
	// Next Monday 00:00
	t := from.Add(24 * time.Hour)
	for t.Weekday() != time.Monday {
		t = t.Add(24 * time.Hour)
	}
	return time.Date(t.Year(), t.Month(), t.Day(), 0, 0, 0, 0, t.Location())
}

func nextMonthly(from time.Time) time.Time {
	y, m, _ := from.Date()
	t := time.Date(y, m+1, 1, 0, 0, 0, 0, from.Location())
	return t
}

func nextYearly(from time.Time) time.Time {
	y, _, _ := from.Date()
	t := time.Date(y+1, 1, 1, 0, 0, 0, 0, from.Location())
	return t
}
