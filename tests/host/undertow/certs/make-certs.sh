#!/bin/sh
# SurfOS Undertow: the certificates of tests/host/test_undertow_transport.c   (10/11/26)
#
# Makes them in this directory, where they are committed with this script, so the tests need no
# OpenSSL. Run it again only to change them (OpenSSL 3.4 or later: -not_before/-not_after); the
# keys are new each time. Test keys and a test authority: they never go under rootfs/ or into any
# image (the browser's own roots are rootfs/etc/ssl/cert.pem).
#
#   ca.pem          Undertow Test Root, RSA 2048, 2026-01-01 to 2046-01-01 (past 2038): the test's trust root
#   int.pem         Undertow Test Intermediate, P-384, signed by the root, 2026-01-01 to 2045-01-01
#   leaf-ec.pem     P-256, signed by the intermediate: fixture.test, www.fixture.test and IP 10.0.2.2,
#                   2026-10-01 to 2030-01-01 (the tests move the clock to make it expired or not yet valid)
#   leaf-rsa.pem    RSA 2048, signed by the root, the same names and dates
#   other-ca.pem    Undertow Unknown Root, P-256: an authority the browser does not trust
#   leaf-unknown.pem  P-256, signed by the unknown root, the same names and dates
#   *.key           their private keys (PKCS#8, unencrypted)
set -eu
cd "$(dirname "$0")"

conf=$(mktemp)
trap 'rm -f "$conf" ./*.csr ./*.srl' EXIT
cat > "$conf" <<'EOF'
[ca]
basicConstraints = critical, CA:TRUE
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
[int]
basicConstraints = critical, CA:TRUE, pathlen:0
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
[leaf]
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = DNS:fixture.test, DNS:www.fixture.test, IP:10.0.2.2
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
EOF

key() {     # key FILE rsa|p256|p384
    case "$2" in
        rsa) openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$1" ;;
        p256) openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$1" ;;
        p384) openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-384 -out "$1" ;;
    esac
}

root() {    # root NAME KEYTYPE CN
    key "$1.key" "$2"
    openssl req -new -x509 -key "$1.key" -subj "/O=SurfOS Undertow tests/CN=$3" -sha256 \
        -not_before 20260101000000Z -not_after 20460101000000Z \
        -config "$conf" -extensions ca -out "$1.pem"
}

signed() {  # signed NAME KEYTYPE CN ISSUER EXTENSIONS NOT_BEFORE NOT_AFTER DIGEST
    key "$1.key" "$2"
    openssl req -new -key "$1.key" -subj "/O=SurfOS Undertow tests/CN=$3" -out "$1.csr"
    openssl x509 -req -in "$1.csr" -CA "$4.pem" -CAkey "$4.key" -CAcreateserial -"$8" \
        -not_before "$6" -not_after "$7" -extfile "$conf" -extensions "$5" -out "$1.pem"
}

root ca rsa "Undertow Test Root"
signed int p384 "Undertow Test Intermediate" ca int 20260101000000Z 20450101000000Z sha256
signed leaf-ec p256 fixture.test int leaf 20261001000000Z 20300101000000Z sha384
signed leaf-rsa rsa fixture.test ca leaf 20261001000000Z 20300101000000Z sha256
root other-ca p256 "Undertow Unknown Root"
signed leaf-unknown p256 fixture.test other-ca leaf 20261001000000Z 20300101000000Z sha256
chmod 644 ./*.pem ./*.key
for f in ca int leaf-ec leaf-rsa other-ca leaf-unknown; do
    openssl x509 -in "$f.pem" -noout -subject -issuer -dates | tr '\n' ' '; echo
done
