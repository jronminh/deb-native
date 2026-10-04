package main

import (
	"fmt"
	"os"
	"shim-systemctl/pkg/sockets"
)

func main() {
	if len(os.Args) < 3 {
		fmt.Println("Usage: sins-socket-activator <unit-path> <service-path>")
		os.Exit(1)
	}

	unitPath := os.Args[1]
	servicePath := os.Args[2]

	activator := sockets.NewActivator(unitPath, servicePath)
	
	fmt.Printf("Starting socket activation for %s -> %s\n", unitPath, servicePath)

	if err := activator.Run(); err != nil {
		fmt.Printf("Manager error: %v\n", err)
		os.Exit(1)
	}
}
