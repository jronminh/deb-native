//go:build dbus

package dbus

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/godbus/dbus/v5"
	"shim-systemctl/pkg/runit"
	"shim-systemctl/pkg/units"
)

func (m *SinsManager) ensureUnitExported(cleanName string) {
	if m.conn == nil {
		return
	}
	path := dbus.ObjectPath("/org/freedesktop/systemd1/unit/" + escapePath(cleanName))
	m.exportMu.Lock()
	defer m.exportMu.Unlock()
	if m.exported[path] {
		return
	}
	u := &unitProps{cleanName: cleanName, rm: m.runitMgr}
	// org.freedesktop.DBus.Properties
	_ = m.conn.Export(u, path, "org.freedesktop.DBus.Properties")
	m.exported[path] = true
}

type unitProps struct {
	cleanName string
	rm        *runit.Manager
}

func findUnitFileForName(cleanName string) string {
	for _, dir := range []string{"/etc/systemd/system", "/usr/lib/systemd/system"} {
		for _, ext := range []string{".service", ".timer", ".socket"} {
			p := filepath.Join(dir, cleanName+ext)
			if st, err := os.Stat(p); err == nil && !st.IsDir() {
				return p
			}
		}
	}
	return ""
}

func (u *unitProps) unitMeta() (description, fragment string) {
	path := findUnitFileForName(u.cleanName)
	if path == "" {
		return "SINS managed service", ""
	}
	fragment = path
	pu, err := units.Parse(path)
	if err != nil {
		return "SINS managed service", fragment
	}
	d := pu.Get("Unit", "Description")
	if d == "" {
		d = "SINS managed service"
	}
	return d, fragment
}

func (u *unitProps) runitStates() (loadState, activeState, subState string) {
	loadState = "loaded"
	sv := filepath.Join(u.rm.ServiceDir, u.cleanName)
	if _, err := os.Stat(sv); err != nil {
		loadState = "not-found"
		return loadState, "inactive", "dead"
	}
	statPath := filepath.Join(sv, "supervise", "stat")
	b, err := os.ReadFile(statPath)
	activeState = "inactive"
	subState = "dead"
	if err != nil {
		return loadState, activeState, subState
	}
	s := strings.TrimSpace(string(b))
	switch {
	case strings.HasPrefix(s, "run"):
		activeState = "active"
		subState = "running"
	case strings.HasPrefix(s, "down"):
		activeState = "inactive"
		subState = "dead"
	case strings.HasPrefix(s, "finish"):
		activeState = "inactive"
		subState = "failed"
	default:
		subState = strings.TrimSpace(s)
	}
	return loadState, activeState, subState
}

// Get implements org.freedesktop.DBus.Properties.Get.
func (u *unitProps) Get(iface, name string) (dbus.Variant, *dbus.Error) {
	all, err := u.GetAll(iface)
	if err != nil {
		return dbus.Variant{}, err
	}
	v, ok := all[name]
	if !ok {
		return dbus.Variant{}, dbus.MakeFailedError(fmt.Errorf("unknown property %s.%s", iface, name))
	}
	return v, nil
}

// GetAll implements org.freedesktop.DBus.Properties.GetAll for org.freedesktop.systemd1.Unit.
func (u *unitProps) GetAll(iface string) (map[string]dbus.Variant, *dbus.Error) {
	if iface != "org.freedesktop.systemd1.Unit" {
		return nil, dbus.MakeFailedError(fmt.Errorf("unsupported interface %s", iface))
	}
	desc, frag := u.unitMeta()
	load, active, sub := u.runitStates()
	id := u.cleanName + ".service"
	out := map[string]dbus.Variant{
		"Id":                dbus.MakeVariant(id),
		"Description":       dbus.MakeVariant(desc),
		"LoadState":         dbus.MakeVariant(load),
		"ActiveState":       dbus.MakeVariant(active),
		"SubState":          dbus.MakeVariant(sub),
		"FragmentPath":      dbus.MakeVariant(frag),
		"UnitFileState":     dbus.MakeVariant("static"),
		"CanStart":          dbus.MakeVariant(true),
		"CanStop":           dbus.MakeVariant(true),
		"CanReload":         dbus.MakeVariant(false),
		"Following":         dbus.MakeVariant(""),
		"RefuseManualStart": dbus.MakeVariant(false),
		"RefuseManualStop":  dbus.MakeVariant(false),
	}
	return out, nil
}

// Set rejects writes (read-only).
func (u *unitProps) Set(iface, name string, val dbus.Variant) *dbus.Error {
	return dbus.MakeFailedError(fmt.Errorf("org.freedesktop.systemd1.Unit properties are read-only under SINS"))
}
