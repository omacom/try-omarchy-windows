import base64
import copy
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("preflight", HERE / "verify-webauthn-preflight.py")
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)

RP_ID = "preflight.try-omarchy.invalid"


def cbor(value) -> bytes:
    def head(major: int, argument: int) -> bytes:
        if argument < 24:
            return bytes([major << 5 | argument])
        for info, size in ((24, 1), (25, 2), (26, 4), (27, 8)):
            if argument < 1 << (8 * size):
                return bytes([major << 5 | info]) + argument.to_bytes(size, "big")
        raise ValueError(argument)

    if isinstance(value, int):
        return head(0, value) if value >= 0 else head(1, -1 - value)
    if isinstance(value, bytes):
        return head(2, len(value)) + value
    if isinstance(value, str):
        encoded = value.encode()
        return head(3, len(encoded)) + encoded
    if isinstance(value, dict):
        return head(5, len(value)) + b"".join(cbor(k) + cbor(v) for k, v in value.items())
    raise TypeError(value)


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


class Authenticator:
    """An ES256 software authenticator built on the openssl command."""

    def __init__(self, folder: pathlib.Path):
        self.folder = folder
        self.key = folder / "key.pem"
        subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(self.key)], check=True)
        der = subprocess.run(["openssl", "pkey", "-in", str(self.key), "-pubout", "-outform", "DER"], check=True, capture_output=True).stdout
        self.x, self.y = der[-64:-32], der[-32:]

    def sign(self, data: bytes) -> bytes:
        (self.folder / "data").write_bytes(data)
        return subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(self.key), str(self.folder / "data")], check=True, capture_output=True).stdout


def auth_data(flags: int, count: int = 0, attested: bytes = b"") -> bytes:
    return hashlib.sha256(RP_ID.encode()).digest() + bytes([flags]) + count.to_bytes(4, "big") + attested


def report(authenticator: Authenticator, get_flags: int = 0x05) -> dict:
    credential_id = b"credential-id-0123"
    cose = cbor({1: 2, 3: -7, -1: 1, -2: authenticator.x, -3: authenticator.y})
    attested = bytes(16) + len(credential_id).to_bytes(2, "big") + credential_id + cose
    attestation = cbor({"fmt": "none", "attStmt": {}, "authData": auth_data(0x45, 0, attested)})
    assertions = []
    for index in range(2):
        client_data = json.dumps({"type": "webauthn.get", "challenge": f"challenge-{index}"}).encode()
        data = auth_data(get_flags, index + 1)
        signature = authenticator.sign(data + hashlib.sha256(client_data).digest())
        assertions.append({"ms": 900, "authenticatorData": b64(data), "signature": b64(signature), "clientData": b64(client_data)})
    return {
        "rpId": RP_ID,
        "result": "signed",
        "create": {"ms": 1200, "credentialId": b64(credential_id), "attestationObject": b64(attestation)},
        "assertions": assertions,
        "delete": "0x0 Success",
    }


class VerifyPreflightTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.authenticator = Authenticator(pathlib.Path(self.temp.name))

    def tearDown(self):
        self.temp.cleanup()

    def test_accepts_verified_signatures(self):
        lines = preflight.verify(report(self.authenticator))
        self.assertEqual(len(lines), 4)
        self.assertIn("approval 2: signature", lines[2])

    def test_rejects_changed_client_data(self):
        changed = report(self.authenticator)
        changed["assertions"][1]["clientData"] = b64(b'{"type":"webauthn.get","challenge":"other"}')
        with self.assertRaisesRegex(preflight.PreflightError, "approval 2: signature"):
            preflight.verify(changed)

    def test_rejects_missing_user_verification(self):
        with self.assertRaisesRegex(preflight.PreflightError, "user verification"):
            preflight.verify(report(self.authenticator, get_flags=0x01))

    def test_rejects_another_relying_party(self):
        other = report(self.authenticator)
        other["rpId"] = "example.com"
        with self.assertRaisesRegex(preflight.PreflightError, "relying party"):
            preflight.verify(other)

    def test_rejects_repeated_challenge(self):
        repeated = report(self.authenticator)
        repeated["assertions"][1] = copy.deepcopy(repeated["assertions"][0])
        with self.assertRaisesRegex(preflight.PreflightError, "challenge repeated"):
            preflight.verify(repeated)

    def test_rejects_unfinished_run(self):
        with self.assertRaisesRegex(preflight.PreflightError, "create-failed"):
            preflight.verify({"result": "create-failed"})


if __name__ == "__main__":
    unittest.main()
