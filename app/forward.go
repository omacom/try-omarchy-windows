package main

import (
	"encoding/base64"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// Loopback-only port forwarding from Windows into the guest, plus the opt-in
// SSH preset built on it. Same semantics as the mac app: a forward binds
// 127.0.0.1 only, the guest service must listen on its network interface,
// and sshd is requested per boot whenever a TCP forward targets guest port
// 22. The launcher never enables sshd across boots or edits its config; the
// guest side lives in the factory overlay's try-omarchy-sshd.service.
type portForward struct {
	proto     string
	hostPort  int
	guestPort int
}

func (f portForward) String() string {
	return fmt.Sprintf("%s:%d:%d", f.proto, f.hostPort, f.guestPort)
}

// parseForward reads the -forward flag form PROTO:HOSTPORT:GUESTPORT, or the
// shorter HOSTPORT:GUESTPORT which means TCP.
func parseForward(value string) (portForward, error) {
	parts := strings.Split(strings.TrimSpace(value), ":")
	f := portForward{proto: "tcp"}
	switch len(parts) {
	case 3:
		f.proto = strings.ToLower(parts[0])
		parts = parts[1:]
	case 2:
	default:
		return f, fmt.Errorf("port forward %q must look like tcp:2222:22", value)
	}
	if f.proto != "tcp" && f.proto != "udp" {
		return f, fmt.Errorf("port forward %q: protocol must be tcp or udp", value)
	}
	var err error
	if f.hostPort, err = parsePort(parts[0]); err != nil {
		return f, fmt.Errorf("port forward %q: Windows port %v", value, err)
	}
	if f.guestPort, err = parsePort(parts[1]); err != nil {
		return f, fmt.Errorf("port forward %q: Omarchy port %v", value, err)
	}
	return f, nil
}

func parsePort(s string) (int, error) {
	n, err := strconv.Atoi(s)
	if err != nil || n < 1 || n > 65535 {
		return 0, fmt.Errorf("%q is not a port between 1 and 65535", s)
	}
	return n, nil
}

// forwardList collects repeated -forward flags and rejects a Windows port
// used twice for the same protocol, which QEMU would otherwise fail on at
// startup with an unhelpful message.
type forwardList []portForward

func (l *forwardList) String() string {
	parts := make([]string, len(*l))
	for i, f := range *l {
		parts[i] = f.String()
	}
	return strings.Join(parts, ",")
}

func (l *forwardList) Set(value string) error {
	f, err := parseForward(value)
	if err != nil {
		return err
	}
	return l.add(f)
}

func (l *forwardList) add(f portForward) error {
	for _, existing := range *l {
		if existing.proto == f.proto && existing.hostPort == f.hostPort {
			return fmt.Errorf("Windows port %d is already forwarded for %s", f.hostPort, f.proto)
		}
	}
	*l = append(*l, f)
	return nil
}

// netdevArg renders the user-mode netdev with every forward bound to the
// loopback address, so nothing on the LAN can reach the guest.
func netdevArg(forwards []portForward) string {
	arg := "user,id=n0"
	for _, f := range forwards {
		arg += fmt.Sprintf(",hostfwd=%s:127.0.0.1:%d-:%d", f.proto, f.hostPort, f.guestPort)
	}
	return arg
}

// sshRequested reports whether any TCP forward targets the guest's sshd.
func sshRequested(forwards []portForward) bool {
	for _, f := range forwards {
		if f.proto == "tcp" && f.guestPort == 22 {
			return true
		}
	}
	return false
}

var publicKeyLine = regexp.MustCompile(`^(ssh-ed25519|ssh-rsa|ecdsa-sha2-nistp(256|384|521)|sk-ssh-ed25519@openssh\.com|sk-ecdsa-sha2-nistp256@openssh\.com) [A-Za-z0-9+/=]+( [^[:cntrl:]]*)?$`)

// loadPublicKey reads one OpenSSH public key line. Anything else, including
// a private key handed over by mistake, is refused before it can reach the
// kernel command line.
func loadPublicKey(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	if len(data) > 4096 {
		return "", fmt.Errorf("%s is too large to be a public key", path)
	}
	line := strings.TrimSpace(strings.ReplaceAll(string(data), "\r\n", "\n"))
	if strings.Contains(line, "\n") {
		return "", fmt.Errorf("%s must contain exactly one public key line", path)
	}
	if strings.Contains(line, "PRIVATE KEY") {
		return "", fmt.Errorf("%s is a private key; pass the .pub file", path)
	}
	if !publicKeyLine.MatchString(line) {
		return "", fmt.Errorf("%s does not look like an OpenSSH public key", path)
	}
	return line, nil
}

// defaultPublicKey finds the user's usual public key when -ssh is used
// without -ssh-key. No key is not an error: password login still works
// until the user hardens sshd inside Omarchy.
func defaultPublicKey(home string) string {
	for _, name := range []string{"id_ed25519.pub", "id_ecdsa.pub", "id_rsa.pub"} {
		path := filepath.Join(home, ".ssh", name)
		if key, err := loadPublicKey(path); err == nil {
			return key
		}
	}
	return ""
}

// resolveSSHPreset folds -ssh and -ssh-key into the forward list and picks
// the public key to authorize. It is the whole decision, kept out of main so
// it can be tested: -ssh adds a TCP forward to port 22; a key path is loaded
// and validated; without one the user's usual key is used if present; and
// -ssh-key without any forward to sshd is a mistake worth reporting.
func resolveSSHPreset(forwards *forwardList, sshPort int, keyPath, home string) (publicKey string, err error) {
	if sshPort != 0 {
		if sshPort < 1 || sshPort > 65535 {
			return "", fmt.Errorf("-ssh needs a Windows port between 1 and 65535")
		}
		if err := forwards.add(portForward{proto: "tcp", hostPort: sshPort, guestPort: 22}); err != nil {
			return "", err
		}
	}
	if !sshRequested(*forwards) {
		if keyPath != "" {
			return "", fmt.Errorf("-ssh-key only makes sense with -ssh or a -forward to Omarchy port 22")
		}
		return "", nil
	}
	if keyPath != "" {
		key, err := loadPublicKey(keyPath)
		if err != nil {
			return "", fmt.Errorf("cannot use the SSH public key: %w", err)
		}
		return key, nil
	}
	if home != "" {
		return defaultPublicKey(home), nil
	}
	return "", nil
}

// sshCmdline renders the kernel command line words the guest's per-boot
// sshd request reads. The key travels base64-encoded because the command
// line is space separated; it is a public key, so its visibility in
// /proc/cmdline is fine.
func sshCmdline(forwards []portForward, publicKey string) string {
	if !sshRequested(forwards) {
		return ""
	}
	words := " tryomarchy.sshd=1"
	if publicKey != "" {
		words += " tryomarchy.sshkey=" + base64.StdEncoding.EncodeToString([]byte(publicKey))
	}
	return words
}
