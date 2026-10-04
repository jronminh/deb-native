package safeunit

import "testing"

func TestValidateServiceName(t *testing.T) {
	for _, ok := range []string{"nginx", "nginx@web", "foo_bar", "svc-1"} {
		if err := ValidateServiceName(ok); err != nil {
			t.Fatalf("expected ok %q: %v", ok, err)
		}
	}
	for _, bad := range []string{"", "..", "a/b", ".hidden", "foo;rm", "foo$(x)"} {
		if err := ValidateServiceName(bad); err == nil {
			t.Fatalf("expected error for %q", bad)
		}
	}
}

func TestValidateEnvKey(t *testing.T) {
	if err := ValidateEnvKey("PATH"); err != nil {
		t.Fatal(err)
	}
	for _, bad := range []string{"../x", "a/b", "9bad"} {
		if err := ValidateEnvKey(bad); err == nil {
			t.Fatalf("expected error for %q", bad)
		}
	}
}
