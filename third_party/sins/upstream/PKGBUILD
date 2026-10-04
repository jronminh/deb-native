# Maintainer: spinty <spinty@example.com>
#
# Build profile (minimal install vs full desktop stack):
#   makepkg    # default: full (same as SINS_PROFILE=full)
#   SINS_PROFILE=minimal makepkg -si   # only systemctl + libsystemd
#   SINS_PROFILE=de makepkg -si        # dbus + notify (typical DE)
#   SINS_PROFILE=server makepkg -si    # dbus + timers + cgroups
# Or exact tags: SINS_TAGS=dbus ./build.sh (see build.sh --list-profiles)
#
pkgname=sins-git
pkgver=0.1
pkgrel=6
pkgdesc="SINS Is Not Systemd - modular systemd-to-runit shim (build: minimal/de/server/full via SINS_PROFILE)"
arch=('x86_64')
url="https://github.com/Spinty-dev/SINS"
license=('MIT')
depends=('runit' 'dbus' 'libelogind')
optdepends=(
  'python: optional helpers such as scripts/sins-journal-cat.py'
  'logrotate: rotate /var/log/sins-journal/journal.sins using contrib snippet'
)
makedepends=('go' 'git' 'gcc')
provides=(
  'systemd'
  'systemd-libs'
  'libsystemd'
  'systemd-sysvcompat'
  'systemctl-shim'
)
conflicts=(
  'systemd'
  'systemd-libs'
  'systemd-sysvcompat'
)
source=("$pkgname::git+https://github.com/Spinty-dev/SINS.git")
md5sums=('SKIP')
install=sins.install

build() {
  cd "$srcdir/$pkgname"
  export SINS_PROFILE="${SINS_PROFILE:-full}"
  ./build.sh
  # Build systemd-analyze stub
  go build -o build/systemd-analyze ./cmd/systemd-analyze
}

package() {
  cd "$srcdir/$pkgname"

  install -Dm755 build/systemctl "$pkgdir/usr/bin/systemctl"
  install -Dm755 build/libsystemd.so.0 "$pkgdir/usr/lib/libsystemd.so.0"
  ln -s libsystemd.so.0 "$pkgdir/usr/lib/libsystemd.so"

  if [[ -f build/sins-daemon ]]; then
    install -Dm755 build/sins-daemon "$pkgdir/usr/bin/sins-daemon"
    install -Dm644 org.freedesktop.systemd1.conf "$pkgdir/usr/share/dbus-1/system.d/org.freedesktop.systemd1.conf"
    install -Dm755 sins-daemon.run "$pkgdir/etc/runit/sv/sins-daemon/run"
  fi
  if [[ -f build/sins-timers ]]; then
    install -Dm755 build/sins-timers "$pkgdir/usr/bin/sins-timers"
  fi
  if [[ -f build/sins-sockets ]]; then
    install -Dm755 build/sins-sockets "$pkgdir/usr/bin/sins-sockets"
  fi
  if [[ -f build/sins-journalctl ]]; then
    install -Dm755 build/sins-journalctl "$pkgdir/usr/bin/sins-journalctl"
  fi

  # systemd-analyze stub
  install -Dm755 build/systemd-analyze "$pkgdir/usr/bin/systemd-analyze"

  # pkg-config for libsystemd compatibility
  install -Dm644 contrib/pkgconfig/libsystemd.pc "$pkgdir/usr/lib/pkgconfig/libsystemd.pc"
  install -Dm644 contrib/pkgconfig/libsystemd.pc "$pkgdir/usr/share/pkgconfig/libsystemd.pc"

  install -d "$pkgdir/var/log/sins-journal"
  install -d "$pkgdir/etc/sins/masked"
  install -Dm644 contrib/logrotate/sins-journal "$pkgdir/etc/logrotate.d/sins-journal"
}
