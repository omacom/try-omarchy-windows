package main

import (
	"bufio"
	"fmt"
	"net"
	"strings"
	"time"
)

// qmpConn is a handshaken QMP connection. A wedged QEMU accepts the TCP
// connect but its main loop never answers, so only a completed greeting +
// qmp_capabilities exchange counts as "QEMU is alive" (the launch watchdog
// depends on that distinction).
type qmpConn struct {
	tcp net.Conn
	r   *bufio.Reader
}

func qmpConnect(port int, readTimeout time.Duration) *qmpConn {
	tcp, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), 3*time.Second)
	if err != nil {
		logf("qmp %d: dial: %v", port, err)
		return nil
	}
	c := &qmpConn{tcp: tcp, r: bufio.NewReader(tcp)}
	tcp.SetReadDeadline(time.Now().Add(readTimeout))
	if _, err := c.r.ReadString('\n'); err != nil { // greeting
		logf("qmp %d: greeting: %v", port, err)
		tcp.Close()
		return nil
	}
	if _, err := tcp.Write([]byte("{\"execute\":\"qmp_capabilities\"}\n")); err != nil {
		logf("qmp %d: caps write: %v", port, err)
		tcp.Close()
		return nil
	}
	tcp.SetReadDeadline(time.Now().Add(readTimeout))
	if _, err := c.r.ReadString('\n'); err != nil { // {"return":{}}
		logf("qmp %d: caps read: %v", port, err)
		tcp.Close()
		return nil
	}
	tcp.SetReadDeadline(time.Time{})
	return c
}

func (c *qmpConn) writeLine(s string) error {
	_, err := c.tcp.Write([]byte(s + "\n"))
	return err
}

func (c *qmpConn) close() { c.tcp.Close() }

// readLines pumps every QMP line (events and command returns alike) into the
// returned channel and closes it when the stream ends. Keeping a read
// permanently pending matters: with -no-reboot, QEMU can exit so fast after a
// guest reset that a poll-style reader loses the SHUTDOWN event to an abortive
// socket close (see docs/FINDINGS.md).
func (c *qmpConn) readLines() <-chan string {
	ch := make(chan string, 16)
	go func() {
		defer close(ch)
		for {
			line, err := c.r.ReadString('\n')
			if line != "" {
				ch <- line
			}
			if err != nil {
				return
			}
		}
	}()
	return ch
}

func shutdownReason(line string) string {
	if !strings.Contains(line, "\"event\"") || !strings.Contains(line, "\"SHUTDOWN\"") {
		return ""
	}
	if strings.Contains(line, "guest-reset") {
		return "reboot"
	}
	return "poweroff"
}
