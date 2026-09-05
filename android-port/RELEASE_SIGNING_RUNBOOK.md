# A9 TAS Android release signing

The public test APK must keep one long-lived signing key. Losing or changing that key
prevents Android from installing future updates over existing tester data.

## Rules

- Keep the keystore outside this source workspace and outside cloud-synced folders.
- Keep two encrypted offline backups in different locations.
- Never put passwords in a script, command history, chat, diagnostic bundle, or repository.
- The ordinary build remains Debug-signed. Release mode is explicit and rejects the
  Android Debug certificate.

## One-time key creation

Use JDK `keytool` interactively. Choose a unique alias and strong passwords; do not use
`android` or `androiddebugkey`.

```powershell
keytool -genkeypair -v -keystore D:\A9TAS-PRIVATE\a9tas-release.p12 `
  -storetype PKCS12 -alias a9tas-release -keyalg RSA -keysize 4096 -validity 10000
```

Record the SHA-256 certificate fingerprint separately:

```powershell
keytool -list -v -keystore D:\A9TAS-PRIVATE\a9tas-release.p12 -alias a9tas-release
```

## Release build

Read passwords without echoing them, expose them only to the current PowerShell process,
and clear them immediately after the build:

```powershell
$storeSecret = Read-Host 'Keystore password' -AsSecureString
$keySecret = Read-Host 'Key password' -AsSecureString
$storePtr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($storeSecret)
$keyPtr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($keySecret)
try {
  $env:A9TAS_RELEASE_STORE_PASSWORD = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($storePtr)
  $env:A9TAS_RELEASE_KEY_PASSWORD = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($keyPtr)
} finally {
  [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($storePtr)
  [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($keyPtr)
}
./android-port/build-g10-android-product-v1.ps1 -SigningMode Release `
  -ReleaseKeystorePath D:\A9TAS-PRIVATE\a9tas-release.p12 `
  -ReleaseKeyAlias a9tas-release
$env:A9TAS_RELEASE_STORE_PASSWORD = $null
$env:A9TAS_RELEASE_KEY_PASSWORD = $null
$storeSecret.Dispose()
$keySecret.Dispose()
```

The resulting APK is written to
`A9TasAndroid/app/build/outputs/apk/release/a9tas-0.7.12-practice-only.apk`.
Archive the APK SHA-256 and `signer_cert_sha256` receipt printed by the build.
