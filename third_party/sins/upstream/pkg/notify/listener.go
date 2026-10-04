//go:build notify
package notify

import (
	"fmt"
	"net"
	"os"
	"path/filepath"
	"shim-systemctl/pkg/safeunit"
	"strings"
	"syscall"
)

const SocketPath = "/run/systemd/notify"

type Listener struct {
	conn       *net.UnixConn
	ServiceDir string
}

func NewListener(serviceDir string) (*Listener, error) {
	dir := filepath.Dir(SocketPath)
	os.MkdirAll(dir, 0755)

	os.Remove(SocketPath)
	addr, err := net.ResolveUnixAddr("unixgram", SocketPath)
	if err != nil {
		return nil, err
	}

	conn, err := net.ListenUnixgram("unixgram", addr)
	if err != nil {
		return nil, err
	}
	_ = os.Chmod(SocketPath, unixSocketModeFromEnv("SINS_NOTIFY_SOCKET_MODE", 0666))

	f, err := conn.File()
	if err == nil {
		syscall.SetsockoptInt(int(f.Fd()), syscall.SOL_SOCKET, syscall.SO_PASSCRED, 1)
	}

	return &Listener{
		conn:       conn,
		ServiceDir: serviceDir,
	}, nil
}

func (l *Listener) Start() {
	buf := make([]byte, 1024)
	oob := make([]byte, 1024)
	for {
		n, oobn, _, _, err := l.conn.ReadMsgUnix(buf, oob)
		if err != nil {
			continue
		}

		msg := string(buf[:n])
		if !strings.Contains(msg, "READY=1") {
			continue
		}

		scms, err := syscall.ParseSocketControlMessage(oob[:oobn])
		if err != nil {
			continue
		}

		for _, scm := range scms {
			if scm.Header.Level == syscall.SOL_SOCKET && scm.Header.Type == syscall.SCM_CREDENTIALS {
				ucred, err := syscall.ParseUnixCredentials(&scm)
				if err == nil {
					l.handleReady(int(ucred.Pid))
				}
			}
		}
	}
}

func unixSocketModeFromEnv(key string, defaultOct uint32) os.FileMode {
	s := strings.TrimSpace(os.Getenv(key))
	if s == "" {
		return os.FileMode(defaultOct)
	}
	var u uint32
	if n, err := fmt.Sscanf(s, "%o", &u); n != 1 || err != nil || u > 07777 {
		return os.FileMode(defaultOct)
	}
	return os.FileMode(u)
}

func (l *Listener) handleReady(pid int) {
	serviceName := l.identifyService(pid)
	if serviceName == "" {
		return
	}
	if err := safeunit.ValidateServiceName(serviceName); err != nil {
		return
	}

	readyFile := filepath.Join(l.ServiceDir, serviceName, ".ready")
	os.WriteFile(readyFile, []byte(fmt.Sprintf("%d", pid)), 0644)
	fmt.Printf("Service %s (PID %d) is READY\n", serviceName, pid)
}

func (l *Listener) identifyService(pid int) string {
	cgroupPath := fmt.Sprintf("/proc/%d/cgroup", pid)
	data, err := os.ReadFile(cgroupPath)
	if err != nil {
		return ""
	}

	lines := strings.Split(string(data), "\n")
	for _, line := range lines {
		if strings.Contains(line, "/sins/") {
			parts := strings.Split(line, "/sins/")
			if len(parts) > 1 {
				return strings.TrimSpace(parts[1])
			}
		}
	}
	return ""
}
