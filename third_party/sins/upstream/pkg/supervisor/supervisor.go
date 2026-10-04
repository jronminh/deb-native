// Package supervisor provides a lightweight process supervisor for user services.
// This replaces the need for a separate runsvdir for user mode.
package supervisor

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"syscall"
	"time"
)

// Service represents a supervised user service.
type Service struct {
	Name     string
	Path     string // Path to service directory (run script)
	Cmd      *exec.Cmd
	Pid      int
	Restart  int
	LastExit int
	mu       sync.RWMutex
	running  bool
	wantUp   bool
}

// Supervisor manages user services without requiring runsvdir.
type Supervisor struct {
	services   map[string]*Service
	mu         sync.RWMutex
	serviceDir string
}

// NewSupervisor creates a supervisor for user services.
func NewSupervisor(serviceDir string) *Supervisor {
	return &Supervisor{
		services:   make(map[string]*Service),
		serviceDir: serviceDir,
	}
}

// StartService starts (or restarts) a service.
func (s *Supervisor) StartService(name string) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	svc, exists := s.services[name]
	if !exists {
		svc = &Service{
			Name:   name,
			Path:   filepath.Join(s.serviceDir, name),
			wantUp: true,
		}
		s.services[name] = svc
	} else {
		svc.wantUp = true
		svc.mu.Lock()
		running := svc.running
		svc.mu.Unlock()
		if running {
			// Already running
			return nil
		}
	}

	return s.spawn(svc)
}

// StopService stops a service.
func (s *Supervisor) StopService(name string) error {
	s.mu.RLock()
	svc, exists := s.services[name]
	s.mu.RUnlock()

	if !exists {
		return fmt.Errorf("service %s not found", name)
	}

	svc.wantUp = false
	svc.mu.Lock()
	cmd := svc.Cmd
	svc.mu.Unlock()

	if cmd != nil && cmd.Process != nil {
		// Try graceful shutdown first
		cmd.Process.Signal(syscall.SIGTERM)

		// Wait up to 5 seconds
		done := make(chan error, 1)
		go func() {
			done <- cmd.Wait()
		}()

		select {
		case <-done:
			return nil
		case <-time.After(5 * time.Second):
			// Force kill
			return cmd.Process.Kill()
		}
	}

	return nil
}

// Status returns the status of a service.
func (s *Supervisor) Status(name string) (string, error) {
	s.mu.RLock()
	svc, exists := s.services[name]
	s.mu.RUnlock()

	if !exists {
		// Check if service exists on disk but not running
		svcPath := filepath.Join(s.serviceDir, name)
		if _, err := os.Stat(svcPath); err == nil {
			return "down", nil
		}
		return "", fmt.Errorf("service %s not found", name)
	}

	svc.mu.RLock()
	running := svc.running
	pid := svc.Pid
	svc.mu.RUnlock()

	if running {
		return fmt.Sprintf("run: %s: (pid %d)", name, pid), nil
	}
	return fmt.Sprintf("down: %s: normally down", name), nil
}

func (s *Supervisor) spawn(svc *Service) error {
	runPath := filepath.Join(svc.Path, "run")
	if _, err := os.Stat(runPath); err != nil {
		return fmt.Errorf("no run script at %s", runPath)
	}

	cmd := exec.Command(runPath)
	cmd.Dir = svc.Path
	cmd.Env = os.Environ()

	// Create log directory if needed
	logPath := filepath.Join(svc.Path, "log", "main", "current")
	os.MkdirAll(filepath.Dir(logPath), 0755)

	// Open log file
	logFile, err := os.OpenFile(logPath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
	if err == nil {
		cmd.Stdout = logFile
		cmd.Stderr = logFile
	}

	if err := cmd.Start(); err != nil {
		return err
	}

	svc.mu.Lock()
	svc.Cmd = cmd
	svc.Pid = cmd.Process.Pid
	svc.running = true
	svc.mu.Unlock()

	// Monitor in background
	go s.monitor(svc)

	return nil
}

func (s *Supervisor) monitor(svc *Service) {
	err := svc.Cmd.Wait()

	svc.mu.Lock()
	svc.running = false
	svc.LastExit = 0
	if err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok {
			svc.LastExit = exitErr.ExitCode()
		}
	}
	svc.Restart++
	svc.mu.Unlock()

	// Auto-restart if wanted and exited non-zero
	if svc.wantUp && svc.LastExit != 0 {
		time.Sleep(1 * time.Second) // Backoff
		s.mu.RLock()
		stillWant := s.services[svc.Name].wantUp
		s.mu.RUnlock()
		if stillWant {
			s.spawn(svc)
		}
	}
}

// ListServices returns all known services.
func (s *Supervisor) ListServices() []string {
	s.mu.RLock()
	defer s.mu.RUnlock()

	var names []string
	for name := range s.services {
		names = append(names, name)
	}
	return names
}

// ScanDirectory finds new services in the service directory.
func (s *Supervisor) ScanDirectory() {
	entries, err := os.ReadDir(s.serviceDir)
	if err != nil {
		return
	}

	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		name := entry.Name()
		s.mu.RLock()
		_, exists := s.services[name]
		s.mu.RUnlock()

		if !exists {
			s.mu.Lock()
			s.services[name] = &Service{
				Name:   name,
				Path:   filepath.Join(s.serviceDir, name),
				wantUp: false,
			}
			s.mu.Unlock()
		}
	}
}
