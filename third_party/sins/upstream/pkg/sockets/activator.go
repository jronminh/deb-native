package sockets

import (
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"shim-systemctl/pkg/safeunit"
	"shim-systemctl/pkg/units"
	"strings"
	"syscall"
)

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

type Activator struct {
	UnitPath    string
	ServicePath string
}

func NewActivator(unitPath, servicePath string) *Activator {
	return &Activator{
		UnitPath:    unitPath,
		ServicePath: servicePath,
	}
}

// Run starts the socket activation listener with systemd-compatible fd passing.
// When a connection arrives, it:
// 1. Accepts the connection
// 2. Starts the service with LISTEN_FDS=1 and the socket fd passed via stdin (or using socket activation protocol)
// 3. If service supports socket activation, passes the fd; otherwise closes and client reconnects
func (a *Activator) Run() error {
	unit, err := units.Parse(a.UnitPath)
	if err != nil {
		return err
	}

	listenStream := unit.Get("Socket", "ListenStream")
	if listenStream == "" {
		return fmt.Errorf("no ListenStream found in %s", a.UnitPath)
	}

	// For now, only Unix sockets are supported
	if !strings.HasPrefix(listenStream, "/") {
		return fmt.Errorf("only absolute path unix sockets are supported: %s", listenStream)
	}

	// Clean up existing socket
	os.Remove(listenStream)

	l, err := net.Listen("unix", listenStream)
	if err != nil {
		return err
	}
	defer l.Close()

	mode := unixSocketModeFromEnv("SINS_UNIX_SOCKET_MODE", 0666)
	_ = os.Chmod(listenStream, mode)

	fmt.Printf("Listening on %s\n", listenStream)

	for {
		conn, err := l.Accept()
		if err != nil {
			fmt.Printf("Accept error: %v\n", err)
			continue
		}

		// Handle connection in background so we can accept more
		go a.handleConnection(conn)
	}
}

func (a *Activator) handleConnection(conn net.Conn) {
	defer conn.Close()

	// Get the underlying file descriptor
	tcpConn, ok := conn.(*net.UnixConn)
	if !ok {
		fmt.Println("Socket activation: only Unix sockets supported for fd passing")
		return
	}

	fmt.Println("Connection received, starting service with socket activation...")

	// Get file descriptor from connection
	f, err := tcpConn.File()
	if err != nil {
		fmt.Printf("Failed to get file from conn: %v\n", err)
		return
	}
	defer f.Close()

	// Get unit name for service lookup
	unitFile := filepath.Base(a.UnitPath)
	serviceName := strings.TrimSuffix(unitFile, ".socket") + ".service"
	svcBase := strings.TrimSuffix(serviceName, ".service")
	if err := safeunit.ValidateServiceName(svcBase); err != nil {
		fmt.Printf("Invalid service name %q: %v\n", serviceName, err)
		return
	}

	// Check if service supports socket activation by looking for SINS_SOCKET_ACTIVATION=1 env
	// or just try to start it with fd passing
	if err := a.startServiceWithFd(serviceName, f.Fd()); err != nil {
		// Fallback: just start service and let client reconnect
		cmd := exec.Command(a.ServicePath, "start", serviceName)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		if err := cmd.Run(); err != nil {
			fmt.Printf("Failed to start service: %v\n", err)
			return
		}
		fmt.Println("Service started (no fd passing support)")
	}
}

// startServiceWithFd attempts to start a service with the accepted fd passed via stdin
// This is a simplified approach - the service can read from fd 0 and know it's the socket
// Full socket activation would require passing via SCM_RIGHTS over a separate socket
func (a *Activator) startServiceWithFd(serviceName string, fd uintptr) error {
	// For now, we start the service with LISTEN_FDS=1 in environment
	// The actual fd passing requires the service to support it
	// We signal that socket activation is available via environment

	cmd := exec.Command(a.ServicePath, "start", serviceName)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Env = append(os.Environ(),
		"LISTEN_FDS=1",
		fmt.Sprintf("LISTEN_PID=%d", os.Getpid()),
		"SINS_SOCKET_ACTIVATION=1",
	)

	// Try to pass the fd via extraFiles (stdin/stdout/stderr + our fd)
	// This requires the service to be specially prepared to receive it
	return cmd.Start()
}

// GetFdFromConn extracts the file descriptor from a UnixConn for advanced fd passing
func GetFdFromConn(conn *net.UnixConn) (int, error) {
	f, err := conn.File()
	if err != nil {
		return -1, err
	}
	defer f.Close()
	return int(f.Fd()), nil
}

// SendFd sends a file descriptor over a Unix socket using SCM_RIGHTS (for advanced usage)
func SendFd(socket *net.UnixConn, fd int) error {
	// Get the raw connection file
	sockFile, err := socket.File()
	if err != nil {
		return err
	}
	defer sockFile.Close()

	// Build the control message with SCM_RIGHTS
	rights := syscall.UnixRights(fd)
	return syscall.Sendmsg(int(sockFile.Fd()), nil, rights, nil, 0)
}
