package main

import (
	"encoding/base64"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestParseForwardAcceptsBothForms(t *testing.T) {
	cases := map[string]portForward{
		"tcp:2222:22":  {"tcp", 2222, 22},
		"udp:5000:600": {"udp", 5000, 600},
		"8080:80":      {"tcp", 8080, 80},
		"TCP:1:65535":  {"tcp", 1, 65535},
	}
	for in, want := range cases {
		got, err := parseForward(in)
		if err != nil || got != want {
			t.Fatalf("parseForward(%q) = %v, %v; want %v", in, got, err, want)
		}
	}
	for _, bad := range []string{"", "22", "sctp:1:2", "tcp:0:22", "tcp:70000:22", "tcp:a:22", "tcp:22:", "tcp:1:2:3"} {
		if _, err := parseForward(bad); err == nil {
			t.Fatalf("parseForward(%q) accepted", bad)
		}
	}
}

func TestForwardListRejectsDuplicateHostPortPerProtocol(t *testing.T) {
	var l forwardList
	for _, v := range []string{"tcp:2222:22", "udp:2222:22", "tcp:8080:80"} {
		if err := l.Set(v); err != nil {
			t.Fatal(err)
		}
	}
	if err := l.Set("tcp:2222:2222"); err == nil {
		t.Fatal("duplicate tcp host port accepted")
	}
	if got := l.String(); got != "tcp:2222:22,udp:2222:22,tcp:8080:80" {
		t.Fatalf("String() = %q", got)
	}
}

func TestNetdevArgBindsLoopbackOnly(t *testing.T) {
	if got := netdevArg(nil); got != "user,id=n0" {
		t.Fatalf("no forwards: %q", got)
	}
	got := netdevArg([]portForward{{"tcp", 2222, 22}, {"udp", 5000, 5000}})
	want := "user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=udp:127.0.0.1:5000-:5000"
	if got != want {
		t.Fatalf("netdevArg = %q, want %q", got, want)
	}
}

func TestSSHCmdlineOnlyForTCPForwardsToPort22(t *testing.T) {
	if got := sshCmdline([]portForward{{"udp", 22, 22}, {"tcp", 8080, 80}}, "ssh-ed25519 AAAA x"); got != "" {
		t.Fatalf("non-ssh forwards requested sshd: %q", got)
	}
	if got := sshCmdline([]portForward{{"tcp", 2222, 22}}, ""); got != " tryomarchy.sshd=1" {
		t.Fatalf("keyless request = %q", got)
	}
	key := "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGxTNqPU2EXAMPLEKEYEXAMPLEKEYEXAMPLEKEYEXAMPLE user@pc"
	got := sshCmdline([]portForward{{"tcp", 2222, 22}}, key)
	prefix := " tryomarchy.sshd=1 tryomarchy.sshkey="
	if !strings.HasPrefix(got, prefix) {
		t.Fatalf("keyed request = %q", got)
	}
	decoded, err := base64.StdEncoding.DecodeString(strings.TrimPrefix(got, prefix))
	if err != nil || string(decoded) != key {
		t.Fatalf("key round trip failed: %q %v", decoded, err)
	}
	for _, word := range strings.Fields(got) {
		if !strings.HasPrefix(word, "tryomarchy.") || strings.ContainsAny(word, "\t\n\"") {
			t.Fatalf("unexpected command line word %q in %q", word, got)
		}
	}
}

func TestLoadPublicKeyRefusesEverythingButOneKeyLine(t *testing.T) {
	dir := t.TempDir()
	write := func(name, content string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
		return p
	}
	good := "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGxTNqPU2EXAMPLEKEYEXAMPLEKEYEXAMPLEKEYEXAMPLE user@pc"
	if key, err := loadPublicKey(write("id_ed25519.pub", good+"\r\n")); err != nil || key != good {
		t.Fatalf("good key rejected: %q %v", key, err)
	}
	bad := map[string]string{
		"private":  "-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n-----END OPENSSH PRIVATE KEY-----\n",
		"twolines": good + "\n" + good + "\n",
		"garbage":  "hello world\n",
		"badtype":  "ssh-dss AAAA user@pc\n",
		"control":  "ssh-ed25519 AAAA user\x07pc\n",
	}
	for name, content := range bad {
		if _, err := loadPublicKey(write(name, content)); err == nil {
			t.Fatalf("%s accepted", name)
		}
	}
	if _, err := loadPublicKey(filepath.Join(dir, "missing.pub")); err == nil {
		t.Fatal("missing file accepted")
	}
}

func TestDefaultPublicKeyPrefersEd25519AndToleratesNone(t *testing.T) {
	home := t.TempDir()
	if got := defaultPublicKey(home); got != "" {
		t.Fatalf("empty home returned %q", got)
	}
	sshDir := filepath.Join(home, ".ssh")
	if err := os.MkdirAll(sshDir, 0o700); err != nil {
		t.Fatal(err)
	}
	rsa := "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQEXAMPLE user@pc"
	ed := "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGxTNqPU2EXAMPLE user@pc"
	os.WriteFile(filepath.Join(sshDir, "id_rsa.pub"), []byte(rsa+"\n"), 0o600)
	if got := defaultPublicKey(home); got != rsa {
		t.Fatalf("rsa fallback = %q", got)
	}
	os.WriteFile(filepath.Join(sshDir, "id_ed25519.pub"), []byte(ed+"\n"), 0o600)
	if got := defaultPublicKey(home); got != ed {
		t.Fatalf("ed25519 preference = %q", got)
	}
}
