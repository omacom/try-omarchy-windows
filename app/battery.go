package main

import (
	"encoding/json"
	"fmt"
)

type systemPowerStatus struct {
	ACLineStatus        byte
	BatteryFlag         byte
	BatteryLifePercent  byte
	Reserved            byte
	BatteryLifeTime     uint32
	BatteryFullLifeTime uint32
}

type batterySnapshot struct {
	Type               string `json:"type"`
	Present            bool   `json:"present"`
	Percentage         *int   `json:"percentage"`
	State              string `json:"state"`
	ACConnected        bool   `json:"acConnected"`
	TimeToEmptySeconds *int   `json:"timeToEmptySeconds"`
	TimeToFullSeconds  *int   `json:"timeToFullSeconds"`
}

func batteryFromWindows(s systemPowerStatus) batterySnapshot {
	b := batterySnapshot{Type: "state", ACConnected: s.ACLineStatus == 1, State: "unknown"}
	// 0xff means unknown, and also sets the no-battery bit. Do not invent a
	// percentage while Windows cannot report one.
	if s.BatteryFlag&0x80 != 0 || s.BatteryLifePercent > 100 {
		return b
	}
	b.Present = true
	percent := int(s.BatteryLifePercent)
	b.Percentage = &percent
	switch {
	case s.BatteryFlag&0x08 != 0:
		b.State = "charging"
	case percent == 100 && b.ACConnected:
		b.State = "full"
	case s.ACLineStatus == 0:
		b.State = "discharging"
	case s.ACLineStatus == 1:
		b.State = "not-charging"
	}
	if b.State == "discharging" && s.BatteryLifeTime != ^uint32(0) {
		seconds := int(s.BatteryLifeTime)
		b.TimeToEmptySeconds = &seconds
	}
	// BatteryFullLifeTime is the total lifetime at full charge, not the
	// time until full. Windows does not supply the latter here.
	return b
}

func encodeBatteryLine(s systemPowerStatus) (string, error) {
	data, err := json.Marshal(batteryFromWindows(s))
	if err != nil {
		return "", fmt.Errorf("encode battery: %w", err)
	}
	return "battery " + string(data) + "\n", nil
}
