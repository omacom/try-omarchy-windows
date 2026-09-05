package main

import (
	"bytes"
	"encoding/binary"
	"image"
	"image/color"
	"image/png"
	"testing"
)

func samplePNG(t *testing.T) ([]byte, *image.NRGBA) {
	t.Helper()
	img := image.NewNRGBA(image.Rect(0, 0, 3, 2))
	img.SetNRGBA(0, 0, color.NRGBA{255, 0, 0, 255})
	img.SetNRGBA(1, 0, color.NRGBA{0, 255, 0, 255})
	img.SetNRGBA(2, 0, color.NRGBA{0, 0, 255, 255})
	img.SetNRGBA(0, 1, color.NRGBA{10, 20, 30, 255})
	img.SetNRGBA(1, 1, color.NRGBA{40, 50, 60, 128})
	img.SetNRGBA(2, 1, color.NRGBA{0, 0, 0, 0})
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes(), img
}

func decodeNRGBA(t *testing.T, data []byte) *image.NRGBA {
	t.Helper()
	img, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	out := image.NewNRGBA(img.Bounds())
	for y := 0; y < img.Bounds().Dy(); y++ {
		for x := 0; x < img.Bounds().Dx(); x++ {
			out.SetNRGBA(x, y, color.NRGBAModel.Convert(img.At(x, y)).(color.NRGBA))
		}
	}
	return out
}

func TestPNGToDIBAndBackKeepsPixels(t *testing.T) {
	data, want := samplePNG(t)
	dib, err := pngToDIB(data)
	if err != nil {
		t.Fatal(err)
	}
	if binary.LittleEndian.Uint16(dib[14:16]) != 32 || int32(binary.LittleEndian.Uint32(dib[8:12])) != 2 || len(dib) != 40+3*4*2 {
		t.Fatalf("unexpected DIB layout: bits=%d height=%d len=%d", binary.LittleEndian.Uint16(dib[14:16]), int32(binary.LittleEndian.Uint32(dib[8:12])), len(dib))
	}
	// Bottom-up: the first stored row is the image's last row, BGRA.
	if !bytes.Equal(dib[40:44], []byte{30, 20, 10, 255}) {
		t.Fatalf("first stored pixel = %v, want BGRA of (10,20,30)", dib[40:44])
	}
	back, err := dibToPNG(dib)
	if err != nil {
		t.Fatal(err)
	}
	got := decodeNRGBA(t, back)
	// A 32-bit BITMAPINFOHEADER DIB carries no alpha mask, so the round trip
	// yields an opaque image with the color channels intact.
	for y := 0; y < 2; y++ {
		for x := 0; x < 3; x++ {
			w := want.NRGBAAt(x, y)
			g := got.NRGBAAt(x, y)
			if g.R != w.R || g.G != w.G || g.B != w.B || g.A != 255 {
				t.Fatalf("pixel %d,%d = %v, want %v opaque", x, y, g, w)
			}
		}
	}
}

func TestDIBToPNGReads24BitTopDownAnd8BitPalette(t *testing.T) {
	dib := make([]byte, 40+4)
	binary.LittleEndian.PutUint32(dib[0:4], 40)
	binary.LittleEndian.PutUint32(dib[4:8], 1)
	binary.LittleEndian.PutUint32(dib[8:12], uint32(0xFFFFFFFF)) // height -1: top-down
	binary.LittleEndian.PutUint16(dib[12:14], 1)
	binary.LittleEndian.PutUint16(dib[14:16], 24)
	copy(dib[40:], []byte{0x30, 0x20, 0x10, 0}) // BGR + row padding
	out, err := dibToPNG(dib)
	if err != nil {
		t.Fatal(err)
	}
	if c := decodeNRGBA(t, out).NRGBAAt(0, 0); c != (color.NRGBA{0x10, 0x20, 0x30, 255}) {
		t.Fatalf("24-bit pixel = %v", c)
	}

	pal := make([]byte, 40+2*4+4)
	binary.LittleEndian.PutUint32(pal[0:4], 40)
	binary.LittleEndian.PutUint32(pal[4:8], 2)
	binary.LittleEndian.PutUint32(pal[8:12], 1)
	binary.LittleEndian.PutUint16(pal[12:14], 1)
	binary.LittleEndian.PutUint16(pal[14:16], 8)
	binary.LittleEndian.PutUint32(pal[32:36], 2)
	copy(pal[40:], []byte{0, 0, 255, 0, 255, 0, 0, 0}) // palette: red, blue (BGRX)
	copy(pal[48:], []byte{1, 0, 0, 0})                 // pixels: blue, red + padding
	out, err = dibToPNG(pal)
	if err != nil {
		t.Fatal(err)
	}
	img := decodeNRGBA(t, out)
	if img.NRGBAAt(0, 0) != (color.NRGBA{0, 0, 255, 255}) || img.NRGBAAt(1, 0) != (color.NRGBA{255, 0, 0, 255}) {
		t.Fatalf("palette pixels = %v %v", img.NRGBAAt(0, 0), img.NRGBAAt(1, 0))
	}
}

func TestDIBToPNGHonorsV5AlphaAndRejectsBadHeaders(t *testing.T) {
	v5 := make([]byte, 124+4)
	binary.LittleEndian.PutUint32(v5[0:4], 124)
	binary.LittleEndian.PutUint32(v5[4:8], 1)
	binary.LittleEndian.PutUint32(v5[8:12], 1)
	binary.LittleEndian.PutUint16(v5[12:14], 1)
	binary.LittleEndian.PutUint16(v5[14:16], 32)
	binary.LittleEndian.PutUint32(v5[16:20], biBitfields)
	binary.LittleEndian.PutUint32(v5[40:44], 0x00ff0000)
	binary.LittleEndian.PutUint32(v5[44:48], 0x0000ff00)
	binary.LittleEndian.PutUint32(v5[48:52], 0x000000ff)
	binary.LittleEndian.PutUint32(v5[52:56], 0xff000000)
	binary.LittleEndian.PutUint32(v5[124:], 0x80112233) // A=128 R=0x11 G=0x22 B=0x33
	out, err := dibToPNG(v5)
	if err != nil {
		t.Fatal(err)
	}
	if c := decodeNRGBA(t, out).NRGBAAt(0, 0); c != (color.NRGBA{0x11, 0x22, 0x33, 128}) {
		t.Fatalf("V5 pixel = %v", c)
	}
	for name, bad := range map[string][]byte{
		"short":       make([]byte, 10),
		"header size": append([]byte{99, 0, 0, 0}, make([]byte, 60)...),
		"huge": func() []byte {
			b := make([]byte, 44)
			binary.LittleEndian.PutUint32(b[0:4], 40)
			binary.LittleEndian.PutUint32(b[4:8], 1<<20)
			binary.LittleEndian.PutUint32(b[8:12], 1)
			binary.LittleEndian.PutUint16(b[14:16], 24)
			return b
		}(),
		"truncated": func() []byte {
			b := make([]byte, 41)
			binary.LittleEndian.PutUint32(b[0:4], 40)
			binary.LittleEndian.PutUint32(b[4:8], 4)
			binary.LittleEndian.PutUint32(b[8:12], 4)
			binary.LittleEndian.PutUint16(b[14:16], 24)
			return b
		}(),
	} {
		if _, err := dibToPNG(bad); err == nil {
			t.Errorf("%s: accepted", name)
		}
	}
	if _, err := pngToDIB([]byte("not a png")); err == nil {
		t.Fatal("non-PNG accepted")
	}
}

func TestClipFramesCarryTextAndImages(t *testing.T) {
	data, _ := samplePNG(t)
	for _, item := range []clipItem{textItem("héllo\n"), pngItem(data)} {
		frame := encodeClipFrame(item)
		back, ok := decodeClipFrame(frame)
		if !ok || back.Kind != item.Kind || !bytes.Equal(back.Data, item.Data) {
			t.Fatalf("%s frame did not round trip", item.Kind)
		}
	}
	if _, ok := decodeClipFrame("png:" + "bm90YXBuZw==\n"); ok {
		t.Fatal("non-PNG bytes accepted as an image")
	}
	if _, ok := decodeClipFrame("png:!!\n"); ok {
		t.Fatal("bad base64 accepted")
	}
	if pngItem(make([]byte, maxClipboardImageBytes+1)).allowed() {
		t.Fatal("oversized image accepted")
	}
	if textItem("a").key() == pngItem(data).key() || pngItem(data).key() != pngItem(append([]byte(nil), data...)).key() {
		t.Fatal("keys must identify content by kind and bytes")
	}
}
