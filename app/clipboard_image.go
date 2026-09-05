package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
)

// Conversion between the Windows CF_DIB clipboard format and PNG. Kept free
// of Win32 calls so the platform-independent tests cover every pixel path.

const (
	dibHeaderSize   = 40
	dibV4HeaderSize = 108
	dibV5HeaderSize = 124
	biRGB           = 0
	biBitfields     = 3
	maxDIBSide      = 16384
)

// dibToPNG decodes a packed DIB (BITMAPINFOHEADER or later, palette, pixel
// rows) into PNG bytes. It covers what Windows apps put on the clipboard:
// 8-bit palette, 24-bit BGR, and 32-bit BGRX or BGRA with default or
// explicit channel masks, top-down or bottom-up.
func dibToPNG(dib []byte) ([]byte, error) {
	if len(dib) < dibHeaderSize {
		return nil, fmt.Errorf("bitmap header truncated")
	}
	headerSize := binary.LittleEndian.Uint32(dib[0:4])
	if headerSize != dibHeaderSize && headerSize != dibV4HeaderSize && headerSize != dibV5HeaderSize || int(headerSize) > len(dib) {
		return nil, fmt.Errorf("unsupported bitmap header size %d", headerSize)
	}
	width := int32(binary.LittleEndian.Uint32(dib[4:8]))
	height := int32(binary.LittleEndian.Uint32(dib[8:12]))
	bits := binary.LittleEndian.Uint16(dib[14:16])
	compression := binary.LittleEndian.Uint32(dib[16:20])
	colorsUsed := binary.LittleEndian.Uint32(dib[32:36])
	topDown := height < 0
	if topDown {
		height = -height
	}
	if width <= 0 || height <= 0 || width > maxDIBSide || height > maxDIBSide {
		return nil, fmt.Errorf("unsupported bitmap size %dx%d", width, height)
	}
	if compression != biRGB && compression != biBitfields {
		return nil, fmt.Errorf("unsupported bitmap compression %d", compression)
	}
	offset := int(headerSize)
	rMask, gMask, bMask, aMask := uint32(0x00ff0000), uint32(0x0000ff00), uint32(0x000000ff), uint32(0)
	if compression == biBitfields {
		masks := dib[offset:]
		if headerSize == dibHeaderSize {
			if len(masks) < 12 {
				return nil, fmt.Errorf("bitmap masks truncated")
			}
			offset += 12
		} else {
			masks = dib[40:]
		}
		rMask = binary.LittleEndian.Uint32(masks[0:4])
		gMask = binary.LittleEndian.Uint32(masks[4:8])
		bMask = binary.LittleEndian.Uint32(masks[8:12])
		if headerSize != dibHeaderSize {
			aMask = binary.LittleEndian.Uint32(dib[52:56])
		}
	} else if headerSize != dibHeaderSize && bits == 32 {
		aMask = binary.LittleEndian.Uint32(dib[52:56])
	}
	var palette []color.NRGBA
	if bits == 8 {
		count := int(colorsUsed)
		if count == 0 || count > 256 {
			count = 256
		}
		if len(dib) < offset+count*4 {
			return nil, fmt.Errorf("bitmap palette truncated")
		}
		palette = make([]color.NRGBA, count)
		for i := range palette {
			p := dib[offset+i*4:]
			palette[i] = color.NRGBA{R: p[2], G: p[1], B: p[0], A: 255}
		}
		offset += count * 4
	} else if bits != 24 && bits != 32 {
		return nil, fmt.Errorf("unsupported bitmap depth %d", bits)
	}
	stride := (int(width)*int(bits) + 31) / 32 * 4
	if len(dib) < offset+stride*int(height) {
		return nil, fmt.Errorf("bitmap pixels truncated")
	}
	img := image.NewNRGBA(image.Rect(0, 0, int(width), int(height)))
	anyAlpha := false
	for y := 0; y < int(height); y++ {
		src := y
		if !topDown {
			src = int(height) - 1 - y
		}
		row := dib[offset+src*stride:]
		for x := 0; x < int(width); x++ {
			var c color.NRGBA
			switch bits {
			case 8:
				index := int(row[x])
				if index >= len(palette) {
					index = 0
				}
				c = palette[index]
			case 24:
				p := row[x*3:]
				c = color.NRGBA{R: p[2], G: p[1], B: p[0], A: 255}
			case 32:
				v := binary.LittleEndian.Uint32(row[x*4:])
				c = color.NRGBA{R: maskedChannel(v, rMask), G: maskedChannel(v, gMask), B: maskedChannel(v, bMask), A: 255}
				if aMask != 0 {
					c.A = maskedChannel(v, aMask)
					if c.A != 0 {
						anyAlpha = true
					}
				}
			}
			img.SetNRGBA(x, y, c)
		}
	}
	if bits == 32 && aMask != 0 && !anyAlpha {
		// Many producers fill the alpha channel with zero while meaning
		// opaque. A fully transparent image is never what was copied.
		for i := 3; i < len(img.Pix); i += 4 {
			img.Pix[i] = 255
		}
	}
	var out bytes.Buffer
	if err := png.Encode(&out, img); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

func maskedChannel(v, mask uint32) uint8 {
	if mask == 0 {
		return 0
	}
	shift := uint(0)
	for mask&(1<<shift) == 0 {
		shift++
	}
	bitsInMask := uint(0)
	for m := mask >> shift; m&1 == 1; m >>= 1 {
		bitsInMask++
	}
	value := (v & mask) >> shift
	if bitsInMask >= 8 {
		return uint8(value >> (bitsInMask - 8))
	}
	return uint8(value * 255 / (1<<bitsInMask - 1))
}

// pngToDIB encodes a PNG as a 32-bit bottom-up BGRA DIB with a plain
// BITMAPINFOHEADER, the form every Windows app reads from CF_DIB.
func pngToDIB(data []byte) ([]byte, error) {
	decoded, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	bounds := decoded.Bounds()
	width, height := bounds.Dx(), bounds.Dy()
	if width <= 0 || height <= 0 || width > maxDIBSide || height > maxDIBSide {
		return nil, fmt.Errorf("unsupported image size %dx%d", width, height)
	}
	stride := width * 4
	out := make([]byte, dibHeaderSize+stride*height)
	binary.LittleEndian.PutUint32(out[0:4], dibHeaderSize)
	binary.LittleEndian.PutUint32(out[4:8], uint32(width))
	binary.LittleEndian.PutUint32(out[8:12], uint32(height))
	binary.LittleEndian.PutUint16(out[12:14], 1)
	binary.LittleEndian.PutUint16(out[14:16], 32)
	binary.LittleEndian.PutUint32(out[16:20], biRGB)
	binary.LittleEndian.PutUint32(out[20:24], uint32(stride*height))
	binary.LittleEndian.PutUint32(out[24:28], 2835) // 72 dpi in pixels per metre
	binary.LittleEndian.PutUint32(out[28:32], 2835)
	for y := 0; y < height; y++ {
		row := out[dibHeaderSize+(height-1-y)*stride:]
		for x := 0; x < width; x++ {
			c := color.NRGBAModel.Convert(decoded.At(bounds.Min.X+x, bounds.Min.Y+y)).(color.NRGBA)
			p := row[x*4:]
			p[0], p[1], p[2], p[3] = c.B, c.G, c.R, c.A
		}
	}
	return out, nil
}
