"""Prepare a separate 4K diagnostic wallpaper module; never edit the input."""
import argparse
import hashlib
import json
from pathlib import Path


def prepare(source):
    # Match only the three shipped background/transition Image elements.
    # Keep their source, crop, animation, alpha and shader paths intact.
    anchor=b'fillMode: Image.PreserveAspectCrop'
    if source.count(anchor)!=3 or b'sourceSize:' in source:
        raise ValueError('Unsupported wallpaper module; inspect before applying the 4K fixture')
    return source.replace(anchor,b'sourceSize: Qt.size(3840, 2160)\n        '+anchor)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    source=args.source.read_bytes()
    if not source or len(source)>128*1024:
        parser.error('Wallpaper source must be a bounded QML file')
    result=prepare(source)
    with args.output.open('xb') as stream:
        stream.write(result)
    print(json.dumps(dict(sourceSha256=hashlib.sha256(source).hexdigest(),outputSha256=hashlib.sha256(result).hexdigest(),
                          decodeWidth=3840,decodeHeight=2160,replacements=3,inputEdited=False,gpuProof=False)))


if __name__=='__main__':
    main()
