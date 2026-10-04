//go:build !notify
package notify

type Listener struct{}

func NewListener(serviceDir string) (*Listener, error) {
	return &Listener{}, nil
}

func (l *Listener) Start() {}
