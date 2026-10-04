package systemctl

import (
	"fmt"
	"os"
	"path/filepath"

	"shim-systemctl/pkg/units"
)

func Cat(ctx *Ctx, unit string) error {
	path := ctx.FindUnitFile(unit)
	if path == "" {
		fmt.Fprintf(os.Stderr, "Unit %s not found.\n", unit)
		return ErrExit{Code: 1}
	}
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	os.Stdout.Write(b)
	if len(b) > 0 && b[len(b)-1] != '\n' {
		fmt.Println()
	}
	return nil
}

func Show(ctx *Ctx, unit string) error {
	path := ctx.FindUnitFile(unit)
	if path == "" {
		fmt.Fprintf(os.Stderr, "Unit %s not found.\n", unit)
		return ErrExit{Code: 1}
	}
	u, err := units.Parse(path)
	if err != nil {
		return err
	}
	base := filepath.Base(path)
	fmt.Printf("Id=%s\n", base)
	fmt.Printf("FragmentPath=%s\n", path)
	for secName, sec := range u.Sections {
		for k, vals := range sec {
			for _, v := range vals {
				fmt.Printf("%s.%s=%s\n", secName, k, v)
			}
		}
	}
	return nil
}

type ErrExit struct {
	Code int
}

func (e ErrExit) Error() string { return "exit" }
