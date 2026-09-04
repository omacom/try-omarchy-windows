package main

import (
	"encoding/base64"
	"strings"
)

// shareLinkName is the name the shared folder gets inside the guest's home
// directory: the Windows folder's own name, so C:\Users\me\Work becomes
// ~/Work. A drive root has no name of its own and becomes its letter.
func shareLinkName(share string) string {
	trimmed := strings.TrimRight(share, `\/`)
	if i := strings.LastIndexAny(trimmed, `\/`); i >= 0 {
		trimmed = trimmed[i+1:]
	}
	if strings.HasSuffix(trimmed, ":") && len(trimmed) == 2 {
		trimmed = strings.ToUpper(trimmed[:1])
	}
	if trimmed == "" || trimmed == "." || trimmed == ".." {
		return "host"
	}
	return trimmed
}

// shareCmdline carries that name to the guest's share-link service. Base64
// keeps spaces and quotes out of the kernel command line.
func shareCmdline(share string) string {
	if share == "" {
		return ""
	}
	return " tryomarchy.sharename=" + base64.StdEncoding.EncodeToString([]byte(shareLinkName(share)))
}
