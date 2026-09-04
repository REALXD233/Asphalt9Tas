param(
    [Parameter(Mandatory=$true)][string]$PrivateKeyPath,
    [Parameter(Mandatory=$true)][ValidateRange(1,365)][int]$Days,
    [string]$DeviceCode = '*'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $PrivateKeyPath -PathType Leaf)) {
    throw 'Private key file not found.'
}
$device = $DeviceCode.Trim().ToLowerInvariant()
if ($device -ne '*' -and $device -notmatch '^[0-9a-f]{32}$') {
    throw 'DeviceCode must be * or the 32-hex code displayed by the APK.'
}
$now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$expires = $now + [int64]$Days * 24 * 60 * 60
$id = [Guid]::NewGuid().ToString('N').Substring(0,16)
$json = '{"v":1,"id":"' + $id + '","nbf":' + $now +
    ',"exp":' + $expires + ',"feature":"tas","device":"' + $device + '"}'
$payload = [Text.Encoding]::UTF8.GetBytes($json)
$key = [Security.Cryptography.ECDsa]::Create()
try {
    $read = 0
    $key.ImportPkcs8PrivateKey([IO.File]::ReadAllBytes($PrivateKeyPath), [ref]$read)
    # Android/Java SHA256withECDSA consumes RFC 3279 ASN.1 DER signatures.
    # .NET's default is IEEE-P1363 (fixed r||s), which Java rejects.
    $signature = $key.SignData($payload,
        [Security.Cryptography.HashAlgorithmName]::SHA256,
        [Security.Cryptography.DSASignatureFormat]::Rfc3279DerSequence)
} finally {
    $key.Dispose()
}
function ConvertTo-Base64Url([byte[]]$Bytes) {
    return [Convert]::ToBase64String($Bytes).TrimEnd('=').Replace('+','-').Replace('/','_')
}
$token = 'A9L1.' + (ConvertTo-Base64Url $payload) + '.' +
    (ConvertTo-Base64Url $signature)
Write-Output $token
Write-Output ('LICENSE_RECEIPT id={0} days={1} device={2} expires={3}' -f `
    $id,$Days,$device,[DateTimeOffset]::FromUnixTimeSeconds($expires).UtcDateTime.ToString('u'))
