//go:build windows

package main

import (
	"encoding/binary"
	"errors"
	"net"
	"syscall"
	"unsafe"
)

var procGetExtendedTcpTable = syscall.NewLazyDLL("iphlpapi.dll").NewProc("GetExtendedTcpTable")

const (
	afInet                    = 2
	tcpTableOwnerPIDConnected = 4
	errInsufficientBuffer     = 122
)

// mibTCPRowOwnerPID mirrors MIB_TCPROW_OWNER_PID. Ports are in network byte
// order in the low 16 bits; addresses are in network byte order.
type mibTCPRowOwnerPID struct {
	state, localAddr, localPort, remoteAddr, remotePort, owningPID uint32
}

// loopbackPeerPID returns the process that owns the other end of an accepted
// IPv4 loopback connection, so a listener can accept only its own QEMU.
func loopbackPeerPID(conn net.Conn) (uint32, error) {
	local, ok1 := conn.LocalAddr().(*net.TCPAddr)
	remote, ok2 := conn.RemoteAddr().(*net.TCPAddr)
	if !ok1 || !ok2 || remote.IP.To4() == nil {
		return 0, errors.New("not an IPv4 TCP connection")
	}
	size := uint32(0)
	procGetExtendedTcpTable.Call(0, uintptr(unsafe.Pointer(&size)), 0, afInet, tcpTableOwnerPIDConnected, 0)
	for attempt := 0; attempt < 4; attempt++ {
		buffer := make([]byte, size+4096)
		size = uint32(len(buffer))
		result, _, _ := procGetExtendedTcpTable.Call(uintptr(unsafe.Pointer(&buffer[0])), uintptr(unsafe.Pointer(&size)), 0, afInet, tcpTableOwnerPIDConnected, 0)
		if result == errInsufficientBuffer {
			continue
		}
		if result != 0 {
			return 0, syscall.Errno(result)
		}
		count := binary.LittleEndian.Uint32(buffer[:4])
		rowSize := uint32(unsafe.Sizeof(mibTCPRowOwnerPID{}))
		if uint64(4)+uint64(count)*uint64(rowSize) > uint64(len(buffer)) {
			return 0, errors.New("TCP table is truncated")
		}
		for i := uint32(0); i < count; i++ {
			row := (*mibTCPRowOwnerPID)(unsafe.Pointer(&buffer[4+i*rowSize]))
			// The peer's row has its own port as the local end and ours as the remote end.
			if networkPort(row.localPort) == remote.Port && networkPort(row.remotePort) == local.Port &&
				networkAddr(row.localAddr).Equal(remote.IP) && networkAddr(row.remoteAddr).Equal(local.IP) {
				return row.owningPID, nil
			}
		}
		return 0, errors.New("the connection's owner was not found")
	}
	return 0, errors.New("TCP table kept growing")
}

func networkPort(value uint32) int {
	return int(uint16(value&0xff)<<8 | uint16(value>>8&0xff))
}

func networkAddr(value uint32) net.IP {
	return net.IPv4(byte(value), byte(value>>8), byte(value>>16), byte(value>>24))
}
