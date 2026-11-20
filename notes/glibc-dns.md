# glibc DNS on Android

**Measured 2026-10-07** on a Samsung Galaxy S23 FE, Android 16, kernel
5.10.240. This follow-up to the Android userspace survey tested whether
a glibc process could open sockets and look up names without bionic's
resolver integration.

- In the app domain (`untrusted_app_27`, target API 28), a prefix
  using patched glibc connected to a remote address. A call to
  `getaddrinfo("example.com")` returned the same address as the system
  resolver.
- The trace showed that the guest's `resolv.conf` and `nsswitch.conf`
  were absent. The lookup succeeded through the device's loopback DNS
  service, which the investigation identified as tethering `dnsmasq`.
- In the shell domain, stock Debian glibc also resolved a real name
  without `resolv.conf` and returned a negative answer for a bogus name.

These measurements establish that name resolution can work with this
device state. Tethering idle behavior and network routing marks were
outside the experiment. [FOA host requirements](https://github.com/jronminh/fhs-on-android/blob/main/reference/domains.md#what-a-host-must-supply)
