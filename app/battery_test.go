package main

import (
	"strings"
	"testing"
)

func TestBatteryFromWindows(t *testing.T) {
	for _, tt := range []struct {
		name   string
		status systemPowerStatus
		want   string
	}{
		{"discharging", systemPowerStatus{ACLineStatus: 0, BatteryFlag: 0, BatteryLifePercent: 57, BatteryLifeTime: 8100}, `"present":true,"percentage":57,"state":"discharging","acConnected":false,"timeToEmptySeconds":8100`},
		{"charging", systemPowerStatus{ACLineStatus: 1, BatteryFlag: 8, BatteryLifePercent: 61, BatteryLifeTime: ^uint32(0)}, `"present":true,"percentage":61,"state":"charging","acConnected":true,"timeToEmptySeconds":null`},
		{"desktop", systemPowerStatus{ACLineStatus: 1, BatteryFlag: 128, BatteryLifePercent: 255}, `"present":false,"percentage":null,"state":"unknown","acConnected":true`},
		{"unknown", systemPowerStatus{ACLineStatus: 255, BatteryFlag: 255, BatteryLifePercent: 255}, `"present":false,"percentage":null,"state":"unknown","acConnected":false`},
	} {
		t.Run(tt.name, func(t *testing.T) {
			line, err := encodeBatteryLine(tt.status)
			if err != nil || !strings.HasPrefix(line, "battery {") || !strings.Contains(line, tt.want) || !strings.HasSuffix(line, "\n") {
				t.Fatalf("battery line %q: %v", line, err)
			}
		})
	}
}
