package main

import (
	"bufio"
	"context"
	"net"
	"slices"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestTimeZoneBridgeSnapshotsChangesAndReconnect(t *testing.T) {
	var zone atomic.Value
	zone.Store("Europe/Berlin")
	source := func() string { return zone.Load().(string) }
	connect := func() (net.Conn, *bufio.Reader, chan time.Time, context.CancelFunc, chan error) {
		host, guest := net.Pipe()
		ctx, cancel := context.WithCancel(context.Background())
		ticks := make(chan time.Time)
		done := make(chan error, 1)
		go func() { done <- serveTimeZoneBridge(ctx, host, source, ticks) }()
		t.Cleanup(func() { cancel(); guest.Close() })
		return guest, bufio.NewReader(guest), ticks, cancel, done
	}
	guest, reader, ticks, cancel, done := connect()
	read := func(want string) {
		t.Helper()
		guest.SetReadDeadline(time.Now().Add(time.Second))
		line, e := reader.ReadString('\n')
		if e != nil || line != want {
			t.Fatal(line, e)
		}
	}
	read("{\"type\":\"timezone\",\"zone\":\"Europe/Berlin\"}\n")
	zone.Store("America/Chicago")
	ticks <- time.Now()
	read("{\"type\":\"timezone\",\"zone\":\"America/Chicago\"}\n")
	ticks <- time.Now()
	read("{\"type\":\"timezone\",\"zone\":\"America/Chicago\"}\n")
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("bridge did not stop")
	}
	guest, reader, _, cancel, done = connect()
	read("{\"type\":\"timezone\",\"zone\":\"America/Chicago\"}\n")
	cancel()
	<-done
}
func TestTimeZoneBridgeRejectsUnknownAndMalformedZones(t *testing.T) {
	for _, zone := range []string{"", "../etc/localtime", "Europe/Berlin\ncommand", "UTC;id", strings.Repeat("a", 65)} {
		if hostTimeZoneSnapshot(zone) != nil {
			t.Fatal("invalid zone sent", zone)
		}
	}
	for _, zone := range []string{"Etc/UTC", "America/Chicago", "Etc/GMT+12"} {
		if hostTimeZoneSnapshot(zone) == nil {
			t.Fatal("valid zone rejected", zone)
		}
	}
	host, guest := net.Pipe()
	defer guest.Close()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		done <- serveTimeZoneBridge(ctx, host, func() string { return "Etc/UTC" }, make(chan time.Time))
	}()
	cancel()
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("cancel did not interrupt blocked writer")
	}
}

func TestTimeZoneChannelRequiresGuestDeclarationAndKeepOptOut(t *testing.T) {
	var spec buildSpec
	if guestAcceptsTimeZone(spec) {
		t.Fatal("old guest accepted live channel")
	}
	spec.Runtime.OptionalDevices = []string{timeZoneDevice}
	if !guestAcceptsTimeZone(spec) {
		t.Fatal("new guest channel unavailable")
	}
	for _, flag := range []string{"", "keep", "Europe/Berlin"} {
		cfg := config{followHostTimeZone: flag != "keep" && guestAcceptsTimeZone(spec)}
		present := slices.Contains(buildQemuArgs(&cfg, ""), "virtserialport,chardev=timezone0,name=dev.tryomarchy.timezone")
		if present != (flag != "keep") {
			t.Fatal("incorrect channel", flag, present)
		}
	}
}
