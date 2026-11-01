#!/usr/bin/env bash
# Decrypt a boot image artifact of the Build workflow on the machine that holds the private key.
#
# The workflow encrypts boot.img with a random key (AES-256-CBC, PBKDF2) and that key with ci/boot-image-recipient.pem
# (RSA-OAEP, SHA-256): the image is built from Apple's kernelcache and is not published. The private key never leaves the
# owner's machine.
#
# usage: ci/decrypt-boot-image.sh <private key PEM> <artifact directory> [output boot.img]
set -euo pipefail
key=$1
dir=$2
out=${3:-$dir/boot.img}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$dir/boot.img.enc" "$dir/boot.key.enc" "$work/"
cp "$key" "$work/private.pem"
(
  cd "$work"   # relative paths: a native openssl.exe on Windows reads them as they are
  openssl pkeyutl -decrypt -inkey private.pem -pkeyopt rsa_padding_mode:oaep -pkeyopt rsa_oaep_md:sha256 \
    -in boot.key.enc -out boot.key
  openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -in boot.img.enc -out boot.img -pass file:boot.key
)
mv "$work/boot.img" "$out"
want=$(cut -d' ' -f1 "$dir/boot.img.sha256")
got=$(sha256sum "$out" | cut -d' ' -f1)
[ "$want" = "$got" ] || { echo "sha256 mismatch: $got, expected $want" >&2; exit 1; }
echo "decrypted $out (sha256 $got)"
