param(
    [string]$IpAddress = "192.168.1.218",
    [int]$Days = 825
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$MainDir = Join-Path $ProjectRoot "main"
$CertPath = Join-Path $MainDir "https_server.crt"
$KeyPath = Join-Path $MainDir "https_server.key"
$ConfigPath = Join-Path ([System.IO.Path]::GetTempPath()) ("matrix_https_{0}.cnf" -f ([System.Guid]::NewGuid().ToString("N")))

$OpenSsl = Get-Command openssl -ErrorAction SilentlyContinue
if ($null -ne $OpenSsl) {
    @"
[req]
default_bits = 2048
prompt = no
default_md = sha256
distinguished_name = dn
x509_extensions = v3_req

[dn]
CN = $IpAddress

[v3_req]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
IP.1 = $IpAddress
"@ | Set-Content -Path $ConfigPath -Encoding ascii

    try {
        & $OpenSsl.Source req -x509 -nodes -days $Days -newkey rsa:2048 -keyout $KeyPath -out $CertPath -config $ConfigPath
    }
    finally {
        Remove-Item -LiteralPath $ConfigPath -Force -ErrorAction SilentlyContinue
    }
}
else {
    $Python = Get-Command python -ErrorAction SilentlyContinue
    if ($null -eq $Python) {
        throw "Neither openssl nor python was found in PATH. Install OpenSSL, or install Python with the cryptography package."
    }

    $PythonPath = Join-Path ([System.IO.Path]::GetTempPath()) ("matrix_https_{0}.py" -f ([System.Guid]::NewGuid().ToString("N")))
    $PythonCode = @'
import datetime
import ipaddress
import sys
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

ip_text, cert_path, key_path, days_text = sys.argv[1:5]
ip = ipaddress.ip_address(ip_text)
days = int(days_text)
key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
subject = issuer = x509.Name([
    x509.NameAttribute(NameOID.COMMON_NAME, ip_text),
])
now = datetime.datetime.now(datetime.timezone.utc)
cert = (
    x509.CertificateBuilder()
    .subject_name(subject)
    .issuer_name(issuer)
    .public_key(key.public_key())
    .serial_number(x509.random_serial_number())
    .not_valid_before(now - datetime.timedelta(minutes=1))
    .not_valid_after(now + datetime.timedelta(days=days))
    .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ip)]), critical=False)
    .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
    .add_extension(
        x509.KeyUsage(
            digital_signature=True,
            key_encipherment=True,
            content_commitment=False,
            data_encipherment=False,
            key_agreement=False,
            key_cert_sign=False,
            crl_sign=False,
            encipher_only=None,
            decipher_only=None,
        ),
        critical=True,
    )
    .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
    .sign(key, hashes.SHA256())
)
Path(cert_path).write_bytes(cert.public_bytes(serialization.Encoding.PEM))
Path(key_path).write_bytes(
    key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,
        serialization.NoEncryption(),
    )
)
'@
    Set-Content -Path $PythonPath -Value $PythonCode -Encoding utf8

    try {
        & $Python.Source $PythonPath $IpAddress $CertPath $KeyPath $Days
    }
    finally {
        Remove-Item -LiteralPath $PythonPath -Force -ErrorAction SilentlyContinue
    }
}

Write-Output "Generated HTTPS certificate: $CertPath"
Write-Output "Generated HTTPS private key: $KeyPath"
Write-Output "Open https://$IpAddress/ and accept/trust the self-signed certificate once in the browser."
