package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"strings"
)

// The bridge carries one clipboard item per line. A bare base64 line is
// UTF-8 text, the original protocol; a line starting with "png:" carries a
// PNG image. Older guests and launchers fail to decode the prefixed form and
// drop it, so the two sides can be upgraded independently.
type clipKind string

const (
	clipText clipKind = "text"
	clipPNG  clipKind = "png"
)

const (
	maxClipboardImageBytes = 16 << 20
	pngFramePrefix         = "png:"
)

var pngSignature = []byte("\x89PNG\r\n\x1a\n")

type clipItem struct {
	Kind clipKind
	Data []byte
}

func textItem(text string) clipItem { return clipItem{Kind: clipText, Data: []byte(text)} }
func pngItem(data []byte) clipItem  { return clipItem{Kind: clipPNG, Data: data} }

func (i clipItem) allowed() bool {
	switch i.Kind {
	case clipText:
		return clipboardTextAllowed(string(i.Data))
	case clipPNG:
		return len(i.Data) > len(pngSignature) && len(i.Data) <= maxClipboardImageBytes && bytes.HasPrefix(i.Data, pngSignature)
	}
	return false
}

// key identifies content for loop prevention without holding a second copy
// of a large image in the sync state.
func (i clipItem) key() string {
	if i.Kind == clipText {
		return "text:" + string(i.Data)
	}
	sum := sha256.Sum256(i.Data)
	return string(i.Kind) + ":" + hex.EncodeToString(sum[:])
}

func encodeClipFrame(i clipItem) string {
	line := base64.StdEncoding.EncodeToString(i.Data)
	if i.Kind == clipPNG {
		line = pngFramePrefix + line
	}
	return line + "\n"
}

// maxClipFrameBytes bounds one incoming line: base64 expands by 4/3, plus the
// prefix and terminator.
const maxClipFrameBytes = (maxClipboardImageBytes+2)/3*4 + len(pngFramePrefix) + 2

func decodeClipFrame(line string) (clipItem, bool) {
	line = strings.TrimRight(line, "\r\n")
	kind := clipText
	if strings.HasPrefix(line, pngFramePrefix) {
		kind = clipPNG
		line = line[len(pngFramePrefix):]
	}
	data, err := base64.StdEncoding.DecodeString(line)
	if err != nil {
		return clipItem{}, false
	}
	item := clipItem{Kind: kind, Data: data}
	return item, item.allowed()
}
