package main

import (
	"fmt"
	"os"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "systemd-analyze: SINS shim - limited functionality")
		fmt.Fprintln(os.Stderr, "Usage: systemd-analyze [command]")
		fmt.Fprintln(os.Stderr, "Supported: --version, -h, --help")
		os.Exit(0)
	}

	switch os.Args[1] {
	case "--version", "version":
		fmt.Println("systemd 256 (SINS shim)")
	case "-h", "--help", "help":
		fmt.Println("systemd-analyze [OPTIONS...] COMMAND")
		fmt.Println()
		fmt.Println("This is a compatibility stub for SINS (SINS Is Not Systemd).")
		fmt.Println("Full systemd-analyze functionality is not available on runit systems.")
		fmt.Println()
		fmt.Println("Supported commands:")
		fmt.Println("  --version    Show version")
		fmt.Println("  -h, --help   Show this help")
	default:
		fmt.Fprintf(os.Stderr, "systemd-analyze: command %q not implemented by SINS shim\n", os.Args[1])
		os.Exit(1)
	}
}
