//go:build !dbus
package dbus

import (
	"github.com/godbus/dbus/v5"
	"shim-systemctl/pkg/runit"
)

type SinsManager struct{}

func Register(conn *dbus.Conn, rm *runit.Manager) error {
	return nil
}
