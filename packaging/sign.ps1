# Code-sign the Gleipnir binaries.
#
# Copyright 2026 ValisSowilo.  GPL-3.0-or-later; see LICENSE.md.
#
#   powershell -ExecutionPolicy Bypass -File packaging\sign.ps1
#   powershell -ExecutionPolicy Bypass -File packaging\sign.ps1 -Thumbprint <hex>
#
# WHAT SIGNING DOES AND DOES NOT DO
#
# With no -Thumbprint this creates a SELF-SIGNED certificate and signs with it.
# Be clear about what that is worth:
#
#   * It removes the "unknown publisher" blank in the UAC/SmartScreen dialog
#     ON MACHINES THAT TRUST THE CERTIFICATE.  By default that is no machine at
#     all, including this one, until the certificate is installed into Trusted
#     Root -- which this script deliberately does not do, because adding a root
#     certificate authority is a real change to a machine's trust model and is
#     not something a build script should do behind anyone's back.
#
#   * It does NOTHING for anyone downloading the release.  Windows SmartScreen
#     and Smart App Control judge on reputation tied to a certificate issued by
#     a CA they already trust.  A self-signed certificate has no reputation and
#     no trusted issuer, so a stranger's machine treats it exactly as it treats
#     an unsigned binary.
#
# So: self-signing is useful for testing the signing pipeline and for your own
# machines.  It is not a fix for the download warning.
#
# WHAT ACTUALLY FIXES THE DOWNLOAD WARNING
#
#   Azure Trusted Signing        ~$10/month, Microsoft's own service, works
#                                immediately.  Cheapest real option; requires
#                                an Azure account and identity verification.
#
#   SignPath Foundation          FREE for OSI-approved open-source projects.
#                                Gleipnir is GPL-3.0-or-later, which is
#                                OSI-approved, so it qualifies.  Start here
#                                before paying anyone.  https://signpath.org/
#
#   Certum Open Source           ~$60/year, aimed at OSS authors, hardware
#                                token posted to you.  A straightforward
#                                fallback if SignPath declines.
#
#   OV/EV certificate            $200-500/year from the usual CAs.  EV gets
#                                instant SmartScreen reputation; OV accrues it
#                                over downloads.
#
# Once you have any of those, pass its thumbprint and this script uses it:
#     .\sign.ps1 -Thumbprint AABBCC...

param(
    [string]$Thumbprint = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$targets = @(
    (Join-Path $root 'dist\gleipnir.exe'),
    (Join-Path $root 'dist\gleipnir-1.0.0-setup.exe')
) | Where-Object { Test-Path $_ }

if (-not $targets) { throw "nothing to sign in $root\dist" }

if ($Thumbprint) {
    $cert = Get-ChildItem Cert:\CurrentUser\My, Cert:\LocalMachine\My |
            Where-Object { $_.Thumbprint -eq $Thumbprint } | Select-Object -First 1
    if (-not $cert) { throw "no certificate with thumbprint $Thumbprint in your stores" }
    Write-Host "signing with: $($cert.Subject)"
} else {
    $subject = "CN=ValisSowilo"
    $cert = Get-ChildItem Cert:\CurrentUser\My |
            Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date) } |
            Select-Object -First 1
    if (-not $cert) {
        Write-Host "creating a self-signed code-signing certificate ($subject)"
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
                    -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(3)
    }
    Write-Host "signing with SELF-SIGNED certificate -- see the notes at the top"
    Write-Host "  of this script: this does not help anyone who downloads Gleipnir."
}

foreach ($t in $targets) {
    # Timestamping matters: without it the signature stops validating the day
    # the certificate expires.  With it, signatures made while the certificate
    # was valid keep validating afterwards.
    $r = Set-AuthenticodeSignature -FilePath $t -Certificate $cert `
             -HashAlgorithm SHA256 -TimestampServer $TimestampUrl -ErrorAction Continue
    "{0,-28} {1}" -f (Split-Path $t -Leaf), $r.Status
}

Write-Host ""
Write-Host "To trust this certificate on THIS machine only (optional, and a"
Write-Host "genuine change to your trust store -- read it before running):"
Write-Host ""
Write-Host "  Export-Certificate -Cert Cert:\CurrentUser\My\$($cert.Thumbprint) -FilePath gleipnir-cert.cer"
Write-Host "  Import-Certificate -FilePath gleipnir-cert.cer -CertStoreLocation Cert:\CurrentUser\Root"
Write-Host ""
Write-Host "Do not ask anyone else to do that. Get a real certificate instead."
