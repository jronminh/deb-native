package main

import (
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"

	"shim-systemctl/pkg/dbus"
	"shim-systemctl/pkg/notify"
	"shim-systemctl/pkg/runit"

	dbus_lib "github.com/godbus/dbus/v5"
)

func main() {
	fmt.Println("SINS Daemon starting... (GNOME Edition v0.1-42)")
	mgr := runit.NewManager()

	// Initialize Notify Listener
	nl, err := notify.NewListener(mgr.ServiceDir)
	if err != nil {
		fmt.Printf("Warning: Failed to start notify listener (likely no permissions): %v\n", err)
	} else {
		fmt.Println("Notify listener started at /run/systemd/notify")
		go nl.Start()
	}

	// Initialize D-Bus Bridge
	var conn *dbus_lib.Conn

	if os.Getenv("SINS_SESSION") != "" {
		fmt.Println("SINS_SESSION detected, connecting to Session Bus...")
		conn, err = dbus_lib.SessionBus()
	} else {
		conn, err = dbus_lib.SystemBus()
		if err != nil {
			fmt.Println("Warning: Could not connect to SystemBus, falling back to SessionBus")
			conn, err = dbus_lib.SessionBus()
		}
	}

	if err != nil {
		log.Fatalf("Failed to connect to D-Bus: %v", err)
	}

	// Request names
	names := []string{
		"org.freedesktop.systemd1",
		"org.freedesktop.hostname1",
		"org.freedesktop.timedate1",
		"org.freedesktop.locale1",
	}

	for _, name := range names {
		reply, err := conn.RequestName(name, dbus_lib.NameFlagReplaceExisting)
		if err != nil {
			fmt.Printf("Warning: Could not request name %s: %v\n", name, err)
		} else if reply != dbus_lib.RequestNameReplyPrimaryOwner {
			fmt.Printf("Warning: Name %s already owned by another process (reply code: %v)\n", name, reply)
		}
	}

	if err := dbus.Register(conn, mgr); err != nil {
		log.Fatalf("Failed to register D-Bus Manager: %v", err)
	}

	// Initialize D-Bus service activation (for apps using dbus-1/services/)
	activationHandler := dbus.NewActivationHandler(os.Getenv("SYSTEMCTL_PATH"))
	if err := activationHandler.LoadServiceFiles(); err != nil {
		fmt.Printf("Warning: Failed to load D-Bus service files: %v\n", err)
	} else {
		services := activationHandler.ListServices()
		if len(services) > 0 {
			fmt.Printf("D-Bus activation: monitoring %d service(s)\n", len(services))
		}
		// Watch for NameOwnerChanged to detect when services need activation
		go dbus.WatchForActivation(conn, activationHandler)
	}

	fmt.Println("SINS Daemon is FULLY ACTIVE")

	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
	<-sigChan

	fmt.Println("Shutting down SINS Daemon...")
}
