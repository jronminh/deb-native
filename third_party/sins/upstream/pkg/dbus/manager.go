//go:build dbus

package dbus

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"

	"github.com/godbus/dbus/v5"
	"github.com/godbus/dbus/v5/introspect"
	"shim-systemctl/pkg/runit"
	"shim-systemctl/pkg/safeunit"
)

func sinsNotImpl(what string) *dbus.Error {
	return dbus.MakeFailedError(fmt.Errorf("%s: not implemented by SINS — configure via your distro (OpenRC/runit hooks, /etc/locale.conf, etc.)", what))
}

type SinsManager struct {
	runitMgr *runit.Manager
	conn     *dbus.Conn
	exportMu sync.Mutex
	exported map[dbus.ObjectPath]bool
}

func Register(conn *dbus.Conn, rm *runit.Manager) error {
	m := &SinsManager{
		runitMgr: rm,
		conn:     conn,
		exported: make(map[dbus.ObjectPath]bool),
	}

	// Register Systemd Manager
	if err := conn.Export(m, "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager"); err != nil {
		return err
	}

	// Register Hostnamed
	if err := conn.Export(m, "/org/freedesktop/hostname1", "org.freedesktop.hostname1"); err != nil {
		return err
	}

	// Register Timedated
	if err := conn.Export(m, "/org/freedesktop/timedate1", "org.freedesktop.timedate1"); err != nil {
		return err
	}

	// Register Localed
	if err := conn.Export(m, "/org/freedesktop/locale1", "org.freedesktop.locale1"); err != nil {
		return err
	}

	node := introspect.Node{
		Name: "/org/freedesktop/systemd1",
		Interfaces: []introspect.Interface{
			introspect.IntrospectData,
			{
				Name: "org.freedesktop.systemd1.Manager",
				Methods: []introspect.Method{
					{Name: "ListUnits", Args: []introspect.Arg{{Name: "units", Type: "a(ssssssouso)", Direction: "out"}}},
					{Name: "StartUnit", Args: []introspect.Arg{{Name: "name", Type: "s", Direction: "in"}, {Name: "mode", Type: "s", Direction: "in"}, {Name: "job", Type: "o", Direction: "out"}}},
					{Name: "StopUnit", Args: []introspect.Arg{{Name: "name", Type: "s", Direction: "in"}, {Name: "mode", Type: "s", Direction: "in"}, {Name: "job", Type: "o", Direction: "out"}}},
					{Name: "RestartUnit", Args: []introspect.Arg{{Name: "name", Type: "s", Direction: "in"}, {Name: "mode", Type: "s", Direction: "in"}, {Name: "job", Type: "o", Direction: "out"}}},
					{Name: "Reload", Args: []introspect.Arg{}},
					{Name: "Subscribe", Args: []introspect.Arg{}},
					{Name: "GetUnit", Args: []introspect.Arg{{Name: "name", Type: "s", Direction: "in"}, {Name: "unit", Type: "o", Direction: "out"}}},
					{Name: "GetUnitByPID", Args: []introspect.Arg{{Name: "pid", Type: "u", Direction: "in"}, {Name: "unit", Type: "o", Direction: "out"}}},
				},
			},
		},
	}

	return conn.Export(introspect.NewIntrospectable(&node), "/org/freedesktop/systemd1", "org.freedesktop.DBus.Introspectable")
}

// Systemd Methods
type UnitInfo struct {
	Name        string
	Description string
	LoadState   string
	ActiveState string
	SubState    string
	Followed    string
	Path        dbus.ObjectPath
	JobId       uint32
	JobType     string
	JobPath     dbus.ObjectPath
}

func escapePath(name string) string {
	res := strings.ReplaceAll(name, "-", "_")
	res = strings.ReplaceAll(res, ".", "_2e")
	return res
}

func (m *SinsManager) ListUnits() ([]UnitInfo, *dbus.Error) {
	files, err := os.ReadDir(m.runitMgr.ServiceDir)
	if err != nil {
		return nil, dbus.MakeFailedError(err)
	}

	var units []UnitInfo
	for _, f := range files {
		if !f.IsDir() || strings.HasPrefix(f.Name(), ".") {
			continue
		}
		name := f.Name()
		if safeunit.ValidateServiceName(name) != nil {
			continue
		}
		activeState := "inactive"
		statusFile := filepath.Join(m.runitMgr.ServiceDir, name, "supervise", "stat")
		if data, err := os.ReadFile(statusFile); err == nil {
			if strings.HasPrefix(string(data), "run") {
				activeState = "active"
			}
		}
		m.ensureUnitExported(name)
		units = append(units, UnitInfo{
			Name: name + ".service", Description: "SINS Managed Service", LoadState: "loaded",
			ActiveState: activeState, SubState: activeState, Path: dbus.ObjectPath("/org/freedesktop/systemd1/unit/" + escapePath(name)),
			JobPath: dbus.ObjectPath("/"),
		})
	}
	return units, nil
}

func (m *SinsManager) StartUnit(name string, mode string) (dbus.ObjectPath, *dbus.Error) {
	_ = mode
	cleanName := strings.TrimSuffix(name, ".service")
	if err := safeunit.ValidateServiceName(cleanName); err != nil {
		return "/", dbus.MakeFailedError(err)
	}
	if err := m.runitMgr.EnableService(cleanName); err != nil {
		return "/", dbus.MakeFailedError(fmt.Errorf("StartUnit enable failed for %s: %w", cleanName, err))
	}
	cmd := exec.Command("sv", "start", cleanName)
	if out, err := cmd.CombinedOutput(); err != nil {
		return "/", dbus.MakeFailedError(fmt.Errorf("StartUnit sv start failed for %s: %v (%s)", cleanName, err, strings.TrimSpace(string(out))))
	}
	return "/", nil
}

func (m *SinsManager) StopUnit(name string, mode string) (dbus.ObjectPath, *dbus.Error) {
	_ = mode
	cleanName := strings.TrimSuffix(name, ".service")
	if err := safeunit.ValidateServiceName(cleanName); err != nil {
		return "/", dbus.MakeFailedError(err)
	}
	cmd := exec.Command("sv", "stop", cleanName)
	if out, err := cmd.CombinedOutput(); err != nil {
		return "/", dbus.MakeFailedError(fmt.Errorf("StopUnit sv stop failed for %s: %v (%s)", cleanName, err, strings.TrimSpace(string(out))))
	}
	return "/", nil
}

func (m *SinsManager) RestartUnit(name string, mode string) (dbus.ObjectPath, *dbus.Error) {
	_ = mode
	cleanName := strings.TrimSuffix(name, ".service")
	if err := safeunit.ValidateServiceName(cleanName); err != nil {
		return "/", dbus.MakeFailedError(err)
	}
	cmd := exec.Command("sv", "restart", cleanName)
	if out, err := cmd.CombinedOutput(); err != nil {
		return "/", dbus.MakeFailedError(fmt.Errorf("RestartUnit sv restart failed for %s: %v (%s)", cleanName, err, strings.TrimSpace(string(out))))
	}
	return "/", nil
}

func (m *SinsManager) GetUnit(name string) (dbus.ObjectPath, *dbus.Error) {
	cleanName := strings.TrimSuffix(name, ".service")
	cleanName = strings.TrimSuffix(cleanName, ".timer")
	cleanName = strings.TrimSuffix(cleanName, ".socket")
	if err := safeunit.ValidateServiceName(cleanName); err != nil {
		return "/", dbus.MakeFailedError(err)
	}
	m.ensureUnitExported(cleanName)
	return dbus.ObjectPath("/org/freedesktop/systemd1/unit/" + escapePath(cleanName)), nil
}

func (m *SinsManager) Reload() *dbus.Error {
	return nil
}

func (m *SinsManager) Subscribe() *dbus.Error {
	return nil
}

func (m *SinsManager) GetUnitByPID(pid uint32) (dbus.ObjectPath, *dbus.Error) {
	_ = pid
	return "/", dbus.MakeFailedError(fmt.Errorf("GetUnitByPID: not implemented by SINS"))
}

// Hostnamed Methods
func (m *SinsManager) SetHostname(name string, interactive bool) *dbus.Error {
	_, _ = name, interactive
	return sinsNotImpl("org.freedesktop.hostname1.SetHostname")
}

func (m *SinsManager) SetStaticHostname(name string, interactive bool) *dbus.Error {
	_, _ = name, interactive
	return sinsNotImpl("org.freedesktop.hostname1.SetStaticHostname")
}

func (m *SinsManager) SetPrettyHostname(name string, interactive bool) *dbus.Error {
	_, _ = name, interactive
	return sinsNotImpl("org.freedesktop.hostname1.SetPrettyHostname")
}

// Timedated Methods
func (m *SinsManager) SetTime(usec int64, relative, interactive bool) *dbus.Error {
	_, _, _ = usec, relative, interactive
	return sinsNotImpl("org.freedesktop.timedate1.SetTime")
}

func (m *SinsManager) SetTimezone(timezone string, interactive bool) *dbus.Error {
	_, _ = timezone, interactive
	return sinsNotImpl("org.freedesktop.timedate1.SetTimezone")
}

func (m *SinsManager) SetLocalRTC(local_rtc, fix_system, interactive bool) *dbus.Error {
	_, _, _ = local_rtc, fix_system, interactive
	return sinsNotImpl("org.freedesktop.timedate1.SetLocalRTC")
}

func (m *SinsManager) SetNTP(use_ntp, interactive bool) *dbus.Error {
	_, _ = use_ntp, interactive
	return sinsNotImpl("org.freedesktop.timedate1.SetNTP")
}

// Localed Methods
func (m *SinsManager) SetLocale(locale []string, interactive bool) *dbus.Error {
	_, _ = locale, interactive
	return sinsNotImpl("org.freedesktop.locale1.SetLocale")
}

func (m *SinsManager) SetVConsoleKeyboard(keymap, keymap_toggle string, convert, interactive bool) *dbus.Error {
	_, _, _, _ = keymap, keymap_toggle, convert, interactive
	return sinsNotImpl("org.freedesktop.locale1.SetVConsoleKeyboard")
}

func (m *SinsManager) SetX11Keyboard(layouts, model, variant, options string, convert, interactive bool) *dbus.Error {
	_, _, _, _, _, _ = layouts, model, variant, options, convert, interactive
	return sinsNotImpl("org.freedesktop.locale1.SetX11Keyboard")
}
