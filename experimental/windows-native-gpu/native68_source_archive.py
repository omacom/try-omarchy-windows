"""Exact historical build bytes; never substitute the current implementation."""
import base64
import hashlib
import json
import zlib

ARCHIVE='docs/evidence/NATIVE68-SOURCE-ARCHIVE-2026-10-21.json'
ARCHIVE_SHA256='9a8bea02e4710c55fc5d080e13f05bf82a020c504060135b36d69c0d67df2ae3'
NATIVE68_BINARY='0f98c12a948693ebb7721d64f700fea4dcf0eb53a9fefd7e82f933286df7a52d'


def native68_sources(repo,receipt):
    raw=(repo/ARCHIVE).read_bytes()
    if hashlib.sha256(raw).hexdigest()!=ARCHIVE_SHA256:
        raise ValueError('Historical Native68 source archive changed')
    value=json.loads(raw)
    if receipt!=value['nativeBuildReceipt'] or receipt.get('bridgeSha256')!=NATIVE68_BINARY:
        raise ValueError('Receipt is not the exact archived Native68 build')
    if len(value['sources'])!=112 or set(value['sources'])!=set(receipt['sources']):
        raise ValueError('Historical Native68 source inventory differs')
    result={}
    for name,row in value['sources'].items():
        source=zlib.decompress(base64.b64decode(row['zlibBase64'],validate=True))
        digest=hashlib.sha256(source).hexdigest()
        if len(source)!=row['bytes'] or digest!=row['sha256'] or digest!=receipt['sources'][name]:
            raise ValueError('Historical Native68 source bytes differ: '+name)
        result[name]=source
    return result
