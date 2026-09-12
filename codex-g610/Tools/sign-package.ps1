<#
.SYNOPSIS
  Run Inf2Cat to generate a driver catalog, then signtool to sign it
  with the test cert.

.DESCRIPTION
  Called by flash.ps1 after staging the driver files into a clean
  directory. Produces $PackageDir\WinMaliKmd.cat and signs both the
  .cat and the .sys with the test cert recorded in
  Tools\.testcert.txt.

  Inf2Cat needs every file referenced by the INF's [SourceDisksFiles]
  to be present alongside the INF before it can hash them.

.PARAMETER PackageDir
  Directory containing the staged driver package.

.PARAMETER OsTargets
  Inf2Cat /os: list. Default covers Win10 RS3+ on x64 and ARM64, which
  is the floor the INF declares ([NTARM64.10.0...16299]).
#>
param(
  [Parameter(Mandatory=$true)][string]$PackageDir,
  # INF is ARM64-only (only [NTARM64.*] model sections), so Inf2Cat must
  # only be given ARM64 OS targets. The 2017 SDK Inf2Cat tops out at RS4
  # but Win11 accepts older signed packages under testsigning.
  [string[]]$OsTargets = @("10_RS3_ARM64","10_RS4_ARM64")
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$thumbFile = Join-Path $scriptDir ".testcert.txt"

if (-not (Test-Path $thumbFile)) {
  throw "No test cert recorded. Run Tools\new-test-cert.ps1 first."
}
$thumb = (Get-Content $thumbFile -Raw).Trim()
$cert = Get-Item "Cert:\CurrentUser\My\$thumb" -ErrorAction SilentlyContinue
if (-not $cert) {
  throw "Cert thumbprint $thumb not found in Cert:\CurrentUser\My."
}
if (-not $cert.HasPrivateKey) {
  throw "Cert thumbprint $thumb has no accessible private key. Run Tools\new-test-cert.ps1."
}
if ($cert.NotBefore -gt (Get-Date) -or $cert.NotAfter -le (Get-Date)) {
  throw "Cert thumbprint $thumb is outside its validity period ($($cert.NotBefore) - $($cert.NotAfter))."
}

# Locate Inf2Cat + signtool. Prefer the SDK version tied to the WDK install.
function Find-Tool([string]$name, [string[]]$archDirs) {
  $root = "C:\Program Files (x86)\Windows Kits\10\bin"
  $candidates = @()
  Get-ChildItem -Path $root -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^10\.0\.' } |
    Sort-Object Name -Descending |
    ForEach-Object {
      foreach ($a in $archDirs) {
        $candidates += (Join-Path $_.FullName "$a\$name")
      }
    }
  foreach ($a in $archDirs) { $candidates += (Join-Path $root "$a\$name") }
  foreach ($c in $candidates) {
    if (Test-Path $c) { return $c }
  }
  throw "$name not found under $root"
}

$Inf2Cat  = Find-Tool "Inf2Cat.exe" @("x86")
$SignTool = Find-Tool "signtool.exe" @("x64","x86")

$packageDirFull = (Resolve-Path $PackageDir).Path
$catPath = Join-Path $packageDirFull "WinMaliKmd.cat"
if (Test-Path $catPath) { Remove-Item -Force $catPath }

# Sign the PE image before Inf2Cat hashes the package.  Reversing this order
# produces a correctly signed catalog whose member hash no longer matches the
# SYS after its embedded Authenticode signature is added.
$sysPath = Join-Path $packageDirFull "WinMaliKmd.sys"
if (-not (Test-Path $sysPath)) {
  throw "Driver package contains no $sysPath"
}
$sysSignOut = & $SignTool sign /fd SHA256 /sha1 $thumb $sysPath 2>&1
$sysSignExit = $LASTEXITCODE
if ($sysSignExit -ne 0) {
  $sysSignOut | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
  throw "signtool failed on $sysPath (exit $sysSignExit)"
}

$osArg = ($OsTargets -join ",")
Write-Host "  inf2cat /driver:`"$packageDirFull`" /os:$osArg" -ForegroundColor DarkGray
$inf2catOut = & $Inf2Cat /driver:"$packageDirFull" /os:$osArg 2>&1
if ($LASTEXITCODE -ne 0) {
  $inf2catOut | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
  throw "Inf2Cat failed (exit $LASTEXITCODE)"
}
if (-not (Test-Path $catPath)) {
  $inf2catOut | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
  throw "Inf2Cat ran but produced no $catPath"
}
$catInfo = Get-Item -LiteralPath $catPath
$catPrefix = [System.IO.File]::ReadAllBytes($catPath) | Select-Object -First 1
if ($catInfo.Length -eq 0 -or $catPrefix -ne 0x30) {
  throw "Inf2Cat produced an invalid catalog (length=$($catInfo.Length), firstByte=$catPrefix)."
}

# Sign the completed catalog last.
$unsignedCatBackup = "$catPath.unsigned"
Copy-Item -LiteralPath $catPath -Destination $unsignedCatBackup -Force
$catSignOut = & $SignTool sign /fd SHA256 /sha1 $thumb $catPath 2>&1
$signExit = $LASTEXITCODE
if ($signExit -ne 0) {
  # Some signing failures may leave the destination partially written.  Keep
  # Inf2Cat's valid output intact so a failed build never emits a zeroed CAT.
  Copy-Item -LiteralPath $unsignedCatBackup -Destination $catPath -Force
  Remove-Item -LiteralPath $unsignedCatBackup -Force
  $catSignOut | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
  throw "signtool failed on $catPath (exit $signExit)"
}

$catalogSignature = Get-AuthenticodeSignature -LiteralPath $catPath
if (-not $catalogSignature.SignerCertificate) {
  Copy-Item -LiteralPath $unsignedCatBackup -Destination $catPath -Force
  Remove-Item -LiteralPath $unsignedCatBackup -Force
  throw "signtool returned success but $catPath contains no detectable signer."
}
Remove-Item -LiteralPath $unsignedCatBackup -Force
$leaf = $catalogSignature.SignerCertificate.Subject
Write-Host "  signed: $(Split-Path $catPath -Leaf) + WinMaliKmd.sys  ($leaf)" -ForegroundColor DarkGray
