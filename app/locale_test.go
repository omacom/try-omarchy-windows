package main

import (
	"strings"
	"testing"
)

func TestIANAZoneForWindowsUsesTheCLDRTable(t *testing.T) {
	for key, want := range map[string]string{
		"Eastern Standard Time": "America/New_York", "W. Europe Standard Time": "Europe/Berlin",
		"UTC": "Etc/UTC", "Tokyo Standard Time": "Asia/Tokyo", "AUS Eastern Standard Time": "Australia/Sydney",
		"Nowhere Standard Time": "", "": "",
	} {
		if got := ianaZoneForWindows(key); got != want {
			t.Errorf("%q: got %q, want %q", key, got, want)
		}
	}
}

func TestXKBForKLIDPrefersFullIdentifiers(t *testing.T) {
	for klid, want := range map[string][2]string{
		"00000409": {"us", ""}, "00020409": {"us", "intl"}, "00010409": {"us", "dvorak"},
		"00000407": {"de", ""}, "0000040C": {"fr", ""}, "00000809": {"gb", ""}, "00000416": {"br", ""},
		"0000080a": {"latam", ""}, "00000419": {"ru", ""}, "00000411": {"jp", ""},
		"0000ffff": {"", ""}, "409": {"", ""}, "": {"", ""},
	} {
		layout, variant := xkbForKLID(klid)
		if layout != want[0] || variant != want[1] {
			t.Errorf("%q: got %q/%q, want %q/%q", klid, layout, variant, want[0], want[1])
		}
	}
}

func TestHostLocaleCmdlineOnlyCarriesValidValues(t *testing.T) {
	if got := hostLocaleCmdline("America/New_York", "de", ""); got != " tryomarchy.tz=America/New_York tryomarchy.kb=de" {
		t.Fatalf("plain: %q", got)
	}
	if got := hostLocaleCmdline("Europe/Berlin", "us", "intl"); got != " tryomarchy.tz=Europe/Berlin tryomarchy.kb=us:intl" {
		t.Fatalf("variant: %q", got)
	}
	if got := hostLocaleCmdline("", "", ""); got != "" {
		t.Fatalf("empty: %q", got)
	}
	for _, bad := range []string{"../etc", "us us", "a b", "Etc UTC", "us;rm"} {
		for _, got := range []string{hostLocaleCmdline(bad, "us", ""), hostLocaleCmdline("Etc/UTC", bad, ""), hostLocaleCmdline("Etc/UTC", "us", bad)} {
			if strings.Contains(got, bad) {
				t.Errorf("unsafe value %q reached the command line: %q", bad, got)
			}
		}
	}
	if got := hostLocaleCmdline("../etc", "us", ""); got != " tryomarchy.kb=us" {
		t.Fatalf("a bad zone must not drop the keyboard: %q", got)
	}
}

func TestSplitKeyboardSpec(t *testing.T) {
	for spec, want := range map[string][2]string{"de": {"de", ""}, " us:intl ": {"us", "intl"}, "": {"", ""}} {
		layout, variant := splitKeyboardSpec(spec)
		if layout != want[0] || variant != want[1] {
			t.Errorf("%q: got %q/%q", spec, layout, variant)
		}
	}
}
