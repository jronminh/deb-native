package units

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type Unit struct {
	Path    string
	Sections map[string]map[string][]string
}

func Parse(path string) (*Unit, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()

	unit := &Unit{
		Path:    path,
		Sections: make(map[string]map[string][]string),
	}

	var currentSection string
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}

		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			currentSection = line[1 : len(line)-1]
			unit.Sections[currentSection] = make(map[string][]string)
			continue
		}

		if currentSection != "" && strings.Contains(line, "=") {
			parts := strings.SplitN(line, "=", 2)
			key := strings.TrimSpace(parts[0])
			val := strings.TrimSpace(parts[1])
			unit.Sections[currentSection][key] = append(unit.Sections[currentSection][key], val)
		}
	}

	return unit, scanner.Err()
}

func (u *Unit) Get(section, key string) string {
	if s, ok := u.Sections[section]; ok {
		if v, ok := s[key]; ok && len(v) > 0 {
			return v[0]
		}
	}
	return ""
}

func (u *Unit) GetAll(section, key string) []string {
	if s, ok := u.Sections[section]; ok {
		return s[key]
	}
	return nil
}

func (u *Unit) GetServiceField(key string) string {
	return u.Get("Service", key)
}

func (u *Unit) ReplacePlaceholders(instance string) {
	prefix := strings.TrimSuffix(filepath.Base(u.Path), filepath.Ext(u.Path))
	if idx := strings.Index(prefix, "@"); idx != -1 {
		prefix = prefix[:idx]
	}

	replacer := strings.NewReplacer(
		"%i", instance,
		"%p", prefix,
		"%n", filepath.Base(u.Path),
	)

	for section := range u.Sections {
		for key := range u.Sections[section] {
			for i := range u.Sections[section][key] {
				u.Sections[section][key][i] = replacer.Replace(u.Sections[section][key][i])
			}
		}
	}
}

func DetectCycles(unitName string, getUnit func(string) (*Unit, error)) error {
	visited := make(map[string]bool)
	stack := make(map[string]bool)

	var check func(string) error
	check = func(name string) error {
		if stack[name] {
			return fmt.Errorf("dependency cycle detected at %s", name)
		}
		if visited[name] {
			return nil
		}

		visited[name] = true
		stack[name] = true
		defer func() { stack[name] = false }()

		u, err := getUnit(name)
		if err != nil || u == nil {
			return nil
		}

		deps := append(u.GetAll("Unit", "After"), u.GetAll("Unit", "Requires")...)
		for _, dep := range deps {
			if err := check(dep); err != nil {
				return err
			}
		}
		return nil
	}

	return check(unitName)
}
