package main

import (
	"encoding/base64"
	"testing"
)

func TestShareLinkNameUsesTheFolderName(t *testing.T) {
	cases := map[string]string{
		`C:\Users\me\Work`:      "Work",
		`C:\Users\me\Work\`:     "Work",
		`C:/Users/me/My Files`:  "My Files",
		`D:\`:                   "D",
		`d:`:                    "D",
		`\\server\share\Photos`: "Photos",
		``:                      "host",
		`.`:                     "host",
	}
	for in, want := range cases {
		if got := shareLinkName(in); got != want {
			t.Fatalf("shareLinkName(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestShareCmdlineEncodesTheNameAsOneWord(t *testing.T) {
	if got := shareCmdline(""); got != "" {
		t.Fatalf("no share produced %q", got)
	}
	got := shareCmdline(`C:\Users\me\My Files`)
	const prefix = " tryomarchy.sharename="
	if len(got) <= len(prefix) || got[:len(prefix)] != prefix {
		t.Fatalf("shareCmdline = %q", got)
	}
	decoded, err := base64.StdEncoding.DecodeString(got[len(prefix):])
	if err != nil || string(decoded) != "My Files" {
		t.Fatalf("round trip = %q %v", decoded, err)
	}
}
