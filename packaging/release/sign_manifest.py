#!/usr/bin/env python3
"""Ed25519-sign manifest.json (writes MANIFEST.sig: base64 of the 64-byte detached signature
over the exact manifest bytes). Adapted from the Kabus22 release tooling.

Key: --key FILE or env JOA_UPDATE_SIGNING_KEY, both base64 of the raw 32-byte seed.
  sign_manifest.py MANIFEST [--key FILE]
  sign_manifest.py --generate-key OUT   (new 0600 key file; prints only the public key)
  sign_manifest.py --public-key [--key FILE]
The private key is never printed. Never commit a key file.
"""
from __future__ import annotations

import argparse
import base64
import os
import sys
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

RAW = dict(encoding=serialization.Encoding.Raw, format=serialization.PublicFormat.Raw)


def public(key: Ed25519PrivateKey) -> str:
    return base64.b64encode(key.public_key().public_bytes(**RAW)).decode()


def load(path: Path | None) -> Ed25519PrivateKey:
    text = path.read_text() if path else os.environ.get("JOA_UPDATE_SIGNING_KEY", "")
    if not text.strip():
        sys.exit("No signing key: pass --key FILE or set JOA_UPDATE_SIGNING_KEY")
    seed = base64.b64decode(text.strip(), validate=True)
    if len(seed) != 32:
        sys.exit("Signing key must be base64 of a raw 32-byte Ed25519 seed")
    return Ed25519PrivateKey.from_private_bytes(seed)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifest", nargs="?", type=Path)
    parser.add_argument("--key", type=Path)
    parser.add_argument("--generate-key", type=Path, metavar="OUT")
    parser.add_argument("--public-key", action="store_true")
    args = parser.parse_args()
    if args.generate_key:
        key = Ed25519PrivateKey.generate()
        seed = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                 serialization.NoEncryption())
        args.generate_key.parent.mkdir(parents=True, exist_ok=True)
        descriptor = os.open(args.generate_key, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, "w") as out:
            out.write(base64.b64encode(seed).decode() + "\n")
        print(public(key))
    elif args.public_key:
        print(public(load(args.key)))
    elif args.manifest:
        signature = load(args.key).sign(args.manifest.read_bytes())
        target = args.manifest.with_name(args.manifest.name + ".sig")
        target.write_text(base64.b64encode(signature).decode() + "\n")
        print(target)
    else:
        parser.error("MANIFEST, --generate-key or --public-key is required")


if __name__ == "__main__":
    main()
