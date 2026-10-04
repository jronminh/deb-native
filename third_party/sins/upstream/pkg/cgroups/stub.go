//go:build !cgroups
package cgroups

type Manager struct{}

func NewManager() *Manager {
	return &Manager{}
}

func (m *Manager) SetupServiceCgroup(name string, limits map[string]string) error {
	return nil
}
