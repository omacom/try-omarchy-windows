#!/usr/bin/env python3
"""Check a webauthn-preflight.json from TryOmarchyWebAuthnPreflight.exe.

The guest verifier will make the same checks on each sudo approval: the
credential's P-256 key from the attestation, the relying party hash, the
user-present and user-verified flags, and an ES256 signature over the
authenticator data and the SHA-256 of the exact client data. Verification
uses the openssl command, as the guest can.
"""

import base64
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

FLAG_USER_PRESENT = 0x01
FLAG_USER_VERIFIED = 0x04
FLAG_ATTESTED_DATA = 0x40
P256_SPKI_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")


class PreflightError(Exception):
    pass


def cbor_item(data: bytes, offset: int = 0):
    """Decode one CBOR item; return (value, next offset). Enough for WebAuthn."""
    if offset >= len(data):
        raise PreflightError("truncated CBOR")
    initial = data[offset]
    major, info = initial >> 5, initial & 0x1F
    offset += 1
    if info < 24:
        argument = info
    elif info in (24, 25, 26, 27):
        size = 1 << (info - 24)
        if offset + size > len(data):
            raise PreflightError("truncated CBOR")
        argument = int.from_bytes(data[offset : offset + size], "big")
        offset += size
    else:
        raise PreflightError("unsupported CBOR length")
    if major == 0:
        return argument, offset
    if major == 1:
        return -1 - argument, offset
    if major in (2, 3):
        if offset + argument > len(data):
            raise PreflightError("truncated CBOR")
        value = data[offset : offset + argument]
        return (value if major == 2 else value.decode("utf-8")), offset + argument
    if major == 4:
        items = []
        for _ in range(argument):
            item, offset = cbor_item(data, offset)
            items.append(item)
        return items, offset
    if major == 5:
        result = {}
        for _ in range(argument):
            key, offset = cbor_item(data, offset)
            value, offset = cbor_item(data, offset)
            result[key] = value
        return result, offset
    if major == 7 and argument in (20, 21, 22):
        return {20: False, 21: True, 22: None}[argument], offset
    raise PreflightError("unsupported CBOR item")


def check_flags(auth_data: bytes, rp_id: str, what: str) -> int:
    if len(auth_data) < 37:
        raise PreflightError(f"{what}: authenticator data is too short")
    if auth_data[:32] != hashlib.sha256(rp_id.encode()).digest():
        raise PreflightError(f"{what}: relying party hash does not match {rp_id}")
    flags = auth_data[32]
    if not flags & FLAG_USER_PRESENT:
        raise PreflightError(f"{what}: user presence flag is not set")
    if not flags & FLAG_USER_VERIFIED:
        raise PreflightError(f"{what}: user verification flag is not set")
    return flags


def credential_key(attestation_object: bytes, credential_id: bytes, rp_id: str) -> bytes:
    attestation, end = cbor_item(attestation_object)
    if end != len(attestation_object) or not isinstance(attestation, dict):
        raise PreflightError("attestation object is not one CBOR map")
    auth_data = attestation.get("authData")
    if not isinstance(auth_data, bytes):
        raise PreflightError("attestation has no authenticator data")
    flags = check_flags(auth_data, rp_id, "create")
    if not flags & FLAG_ATTESTED_DATA:
        raise PreflightError("create: no attested credential data")
    offset = 37 + 16
    id_length = int.from_bytes(auth_data[offset : offset + 2], "big")
    offset += 2
    if auth_data[offset : offset + id_length] != credential_id:
        raise PreflightError("create: credential ID does not match")
    key, _ = cbor_item(auth_data, offset + id_length)
    if not isinstance(key, dict) or key.get(1) != 2 or key.get(3) != -7 or key.get(-1) != 1:
        raise PreflightError("create: credential key is not an ES256 P-256 key")
    x, y = key.get(-2), key.get(-3)
    if not isinstance(x, bytes) or not isinstance(y, bytes) or len(x) != 32 or len(y) != 32:
        raise PreflightError("create: credential key coordinates are malformed")
    return P256_SPKI_PREFIX + b"\x04" + x + y


def verify_es256(public_key_der: bytes, signed: bytes, signature: bytes) -> bool:
    with tempfile.TemporaryDirectory() as directory:
        folder = pathlib.Path(directory)
        (folder / "key.der").write_bytes(public_key_der)
        (folder / "signed").write_bytes(signed)
        (folder / "signature").write_bytes(signature)
        result = subprocess.run(
            [
                "openssl", "dgst", "-sha256",
                "-verify", str(folder / "key.der"), "-keyform", "DER",
                "-signature", str(folder / "signature"), str(folder / "signed"),
            ],
            capture_output=True,
            text=True,
        )
        return result.returncode == 0


def verify(report: dict) -> list[str]:
    if report.get("result") != "signed":
        raise PreflightError(f"preflight did not finish: {report.get('result')}")
    rp_id = report["rpId"]
    create = report["create"]
    credential_id = base64.b64decode(create["credentialId"])
    public_key = credential_key(base64.b64decode(create["attestationObject"]), credential_id, rp_id)
    lines = [f"create: ES256 key pinned ({create['ms']} ms)"]
    challenges = set()
    for index, assertion in enumerate(report["assertions"], 1):
        what = f"approval {index}"
        auth_data = base64.b64decode(assertion["authenticatorData"])
        client_data = base64.b64decode(assertion["clientData"])
        check_flags(auth_data, rp_id, what)
        challenge = json.loads(client_data)["challenge"]
        if challenge in challenges:
            raise PreflightError(f"{what}: challenge repeated")
        challenges.add(challenge)
        signed = auth_data + hashlib.sha256(client_data).digest()
        if not verify_es256(public_key, signed, base64.b64decode(assertion["signature"])):
            raise PreflightError(f"{what}: signature does not verify")
        count = int.from_bytes(auth_data[33:37], "big")
        lines.append(f"{what}: signature, user presence and verification OK "
                     f"(sign count {count}, {assertion['ms']} ms)")
    if len(challenges) != 2:
        raise PreflightError("expected two approvals")
    lines.append(f"delete: {report.get('delete')}")
    return lines


def main() -> int:
    path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "webauthn-preflight.json")
    try:
        for line in verify(json.loads(path.read_text(encoding="utf-8"))):
            print(line)
    except (PreflightError, KeyError, ValueError) as error:
        print(f"FAILED: {error}")
        return 1
    print("PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
