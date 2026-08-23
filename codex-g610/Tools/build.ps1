<#
.SYNOPSIS
    Build the WinMali-rk3588 solution.

.DESCRIPTION
    Top-level orchestrator. Wraps MSBuild for the .vcxproj projects and
    calls build-mesa.ps1 for the meson side.

    Examples:
        .\Tools\build.ps1                          # Debug, auto platform, everything
        .\Tools\build.ps1 -Configuration Release
        .\Tools\build.ps1 -Project Kmd             # Just the KMD
        .\Tools\build.ps1 -Project Mesa            # Just the mesa ICD
        .\Tools\build.ps1 -SkipMesa                # KMD + UMD, no mesa
        .\Tools\build.ps1 -DriverLogProfile Focused # concise bring-up logs
        .\Tools\build.ps1 -DriverLogProfile Lifecycle # load/start/stop only
        .\Tools\build.ps1 -Rebuild

    Exits non-zero if any step fails so CI can chain.

.PARAMETER Configuration
    Debug (default) or Release.

.PARAMETER Platform
    ARM64 / x64 / auto. auto picks ARM64 if the WDK's ARM64 libs are
    installed, x64 otherwise (compile-test only).

.PARAMETER Project
    All (default), Kmd, Umd, Mesa.

.PARAMETER SkipMesa
    Skip the mesa step regardless of -Project.

.PARAMETER Rebuild
    /t:Rebuild instead of /t:Build (and -Reconfigure for mesa).

.PARAMETER SignMode
    Off (default), TestSign, LabCert. TestSign assembles the complete INF
    payload, generates its catalog, and signs the catalog and SYS with the
    certificate recorded by new-test-cert.ps1.

.PARAMETER DriverLogProfile
    Verbose keeps all historical trace output. Focused compiles out routine
    success-path chatter while retaining faults, warnings, lifecycle events,
    scheduler/fence transitions, synchronization, and DXGI presentation logs.
    Lifecycle suppresses all KMD and Mesa/UMD output except KMD driver load,
    StartDevice, StopDevice, RemoveDevice, and unload messages.

.PARAMETER InfVerif
    Enable InfVerif. Off by default (frequent missing on dev boxes).

.PARAMETER ApiValidate
    Enable ApiValidator. Off by default (same reason).
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug","Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("ARM64","x64","auto")]
    [string]$Platform = "auto",

    [ValidateSet("All","Kmd","Mesa")]
    [string]$Project = "All",

    [switch]$SkipMesa,
    [switch]$Rebuild,
    [switch]$StopOnFirstError,

    [ValidateSet("quiet","minimal","normal","detailed","diagnostic")]
    [string]$LogLevel = "minimal",

    [ValidateSet("Verbose","Focused","Lifecycle")]
    [string]$DriverLogProfile = "Verbose",

    [switch]$InfVerif,
    [switch]$ApiValidate,

    [ValidateSet("Off","TestSign","LabCert")]
    [string]$SignMode = "Off"
)

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\env.ps1"
Show-WinMaliEnv

# Generate one version for the entire invocation and feed the same four
# components to StampInf and the SYS VERSIONINFO resource. Windows ranks a
# Display driver by DriverVer date/version; reusing the old static version made
# Device Manager correctly report that an equal-ranked package was already
# installed. YY.DayOfYear.MinuteOfDay.Second is monotonic and each component
# fits the 16-bit Windows file-version fields.
$driverVersionTime = Get-Date
$driverVersionParts = @(
    ($driverVersionTime.Year % 100),
    $driverVersionTime.DayOfYear,
    (($driverVersionTime.Hour * 60) + $driverVersionTime.Minute),
    $driverVersionTime.Second
)
$driverVersion = $driverVersionParts -join "."

if (-not $WinMaliMsBuild) {
    Write-Host "FATAL: MSBuild not found. Install Visual Studio (2017+) + C++ workload." -ForegroundColor Red
    exit 2
}
if (-not $WinMaliWdkRoot) {
    Write-Host "FATAL: Windows Driver Kit not installed." -ForegroundColor Red
    Write-Host "  https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk" -ForegroundColor Yellow
    exit 3
}

# Load the development environment for the same current WDK selected by the
# KMD project. Current upstream uses WDDM 3.2 declarations from WDK 26100;
# forcing the old 17763 headers here makes those declarations disappear.
if (-not (Enter-WinMaliVsDevEnv -WinSdk $WinMaliWdkVersion)) {
    Write-Host "FATAL: failed to enter VS Dev environment." -ForegroundColor Red
    exit 5
}

# --- Resolve auto platform -------------------------------------------------
if ($Platform -eq "auto") {
    $arm64Dir = Join-Path $WinMaliWdkRoot ("lib\$WinMaliWdkVersion\um\arm64")
    $arm64Ok  = (Test-Path $arm64Dir) -and ((Get-ChildItem $arm64Dir -Filter *.lib -EA SilentlyContinue).Count -gt 0)
    if ($arm64Ok) {
        $Platform = "ARM64"
        Write-Host "auto-platform: ARM64 (WDK ARM64 libs present)" -ForegroundColor DarkCyan
    } else {
        $Platform = "x64"
        Write-Host "auto-platform: x64 (no ARM64 WDK libs - compile-test only)" -ForegroundColor Yellow
    }
}

# --- MSBuild projects ------------------------------------------------------
$msbuildTarget = if ($Rebuild) { "Rebuild" } else { "Build" }
$msbuildResults = @()

function Invoke-MsBuild([string]$projOrSln, [string]$label) {
    # Always rebuild the KMD so a new generated version cannot leave a stale
    # incremental INF or VERSIONINFO resource behind.
    $projectTarget = if ($label -eq "WinMaliKmd") { "Rebuild" } else { $msbuildTarget }
    $args = @(
        $projOrSln,
        "/t:$projectTarget",
        "/p:Configuration=$Configuration",
        "/p:Platform=$Platform",
        "/nologo",
        "/v:$LogLevel",
        "/clp:ShowTimestamp;Summary"
    )

    if ($label -eq "WinMaliKmd") {
        $args += @(
            "/p:WinMaliVersionMajor=$($driverVersionParts[0])",
            "/p:WinMaliVersionMinor=$($driverVersionParts[1])",
            "/p:WinMaliVersionBuild=$($driverVersionParts[2])",
            "/p:WinMaliVersionRevision=$($driverVersionParts[3])",
            "/p:WinMaliLogProfile=$DriverLogProfile"
        )
    }

    if ($StopOnFirstError) { $args += "/p:StopOnFirstFailure=true" }
    if (-not $InfVerif)    { $args += "/p:SkipPackageVerification=true" }
    if (-not $ApiValidate) { $args += "/p:ApiValidator_Enable=false" }

    # The KMD INF references Mesa/UMD payloads that are produced outside this
    # vcxproj.  WDK's in-project Inf2Cat target runs before build.ps1 can
    # assemble those files and therefore validates an incomplete project
    # package.  For TestSign, compile unsigned here and sign the complete
    # staged package below.  LabCert retains the native WDK behavior.
    $msbuildSignMode = if ($label -eq "WinMaliKmd" -and $SignMode -eq "TestSign") {
        "Off"
    } else {
        $SignMode
    }
    $args += "/p:SignMode=$msbuildSignMode"

    if ($msbuildSignMode -eq "Off") {
        $args += "/p:EnableInf2cat=false"
    }

    Write-Host ""
    Write-Host "== $label ($Configuration|$Platform, $projectTarget) ==" -ForegroundColor Cyan
    if ($label -eq "WinMaliKmd") {
        Write-Host "DriverVer: $($driverVersionTime.ToString('MM/dd/yyyy')),$driverVersion" -ForegroundColor DarkCyan
    }

    # Out-Host so MSBuild's output renders to the console immediately and
    # does NOT become part of the function's return value (which would
    # otherwise pollute the exit-code-only return contract).
    & $WinMaliMsBuild @args | Out-Host

    $code = $LASTEXITCODE
    $script:msbuildResults += [pscustomobject]@{
        Label = $label
        Code  = $code
    }

    return $code
}

$runMesa = ($Project -in @("All","Mesa")) -and (-not $SkipMesa)
$runKmd  = ($Project -in @("All","Kmd"))

$overall = 0

# Mesa first: KMD's INF depends on WinMaliGL.dll being present in
# the staging dir at package-time when InfVerif/Inf2cat run.
#
# ARM64 packages currently contain native ARM64 user-mode binaries only.
# Win32/WOW64 support is intentionally deferred until the native render path
# is stable, so an ARM64 build must not spend time building or staging it.
function Invoke-MesaBuild([string]$mesaPlatform) {
    Write-Host ""
    Write-Host "== Mesa ($Configuration|$mesaPlatform) ==" -ForegroundColor Cyan
    $mesaArgs = @{
        Configuration = $Configuration
        Platform = $mesaPlatform
        DriverLogProfile = $DriverLogProfile
    }
    # Always let Meson/Ninja perform its incremental dependency check. Merely
    # finding old output DLLs does not mean they contain current sources; using
    # SkipIfPresent here previously allowed a newly edited UMD to be packaged
    # without compiling it. -Rebuild additionally recreates the Meson tree.
    if ($Rebuild) { $mesaArgs.Reconfigure = $true }
    # Keep the function's return contract to one integer.  Otherwise every
    # line emitted by Meson becomes part of $code in the caller.
    & $mesaScript @mesaArgs | Out-Host
    return $LASTEXITCODE
}

function Copy-VulkanIcdJsons([string]$nativePlatform) {
    # Drop the native Vulkan loader manifest alongside the binaries so
    # the INF's CopyFiles section finds them in the source dir at
    # Inf2cat / staging time.
    $srcDir = Join-Path $PSScriptRoot "..\KMD"
    $dstDir = Join-Path $WinMaliBuildDir "bin\$Configuration\$nativePlatform"
    New-Item -ItemType Directory -Force -Path $dstDir | Out-Null
    foreach ($json in @("WinMaliVk_icd.json")) {
        $src = Join-Path $srcDir $json
        if (Test-Path $src) {
            Copy-Item -Path $src -Destination (Join-Path $dstDir $json) -Force
            Write-Host "Staged Vulkan ICD manifest: $dstDir\$json" -ForegroundColor DarkGreen
        } else {
            Write-Host "Vulkan ICD manifest missing: $src" -ForegroundColor Yellow
        }
    }
}

function New-TestSignedPackage([string]$packagePlatform) {
    $binDir = Join-Path $WinMaliBuildDir "bin\$Configuration\$packagePlatform"
    $stageDir = Join-Path $WinMaliBuildDir "stage\$Configuration\$packagePlatform\WinMaliKmd"
    $required = @(
        "WinMaliKmd.sys",
        "WinMaliKmd.inf",
        "WinMaliUmd.dll",
        "WinMaliGL.dll",
        "WinMaliVk.dll",
        "WinMaliVk_icd.json"
    )
    # These are the owner-provided ARM64 runtime/firmware payloads. They are
    # referenced by the INF but intentionally are not rebuilt by this repo.
    # WINMALI_RUNTIME_PAYLOAD_DIR permits an explicit mirror; otherwise use
    # the GpuDrive folder supplied with this checkout.
    $runtimePayloadDir = $env:WINMALI_RUNTIME_PAYLOAD_DIR
    if (-not $runtimePayloadDir) {
        $runtimePayloadDir = Join-Path (Split-Path -Parent $WinMaliRoot) "Downloads\GpuDrive"
    }
    $runtimeRequired = @(
        "vcruntime140.dll",
        "msvcp140.dll",
        "z-1.dll",
        "mali_csffw.bin"
    )

    $missing = @($required | Where-Object { -not (Test-Path (Join-Path $binDir $_)) })
    if ($missing.Count -ne 0) {
        Write-Host "FATAL: cannot test-sign an incomplete INF package in $binDir" -ForegroundColor Red
        $missing | ForEach-Object { Write-Host "  missing: $_" -ForegroundColor Red }
        Write-Host "Build the full ARM64 package once without -Project Kmd so Mesa/UMD payloads are staged." -ForegroundColor Yellow
        return 1
    }
    $missingRuntime = @($runtimeRequired | Where-Object {
        -not (Test-Path (Join-Path $runtimePayloadDir $_))
    })
    if ($missingRuntime.Count -ne 0) {
        Write-Host "FATAL: owner runtime payload is incomplete: $runtimePayloadDir" -ForegroundColor Red
        $missingRuntime | ForEach-Object { Write-Host "  missing: $_" -ForegroundColor Red }
        return 1
    }

    if (Test-Path $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
    foreach ($name in $required) {
        Copy-Item -LiteralPath (Join-Path $binDir $name) `
                  -Destination (Join-Path $stageDir $name) -Force
    }
    foreach ($name in $runtimeRequired) {
        Copy-Item -LiteralPath (Join-Path $runtimePayloadDir $name) `
                  -Destination (Join-Path $stageDir $name) -Force
    }

    Write-Host ""
    Write-Host "== Test-sign complete driver package ==" -ForegroundColor Cyan
    Write-Host "  Package: $stageDir" -ForegroundColor DarkGray
    & (Join-Path $PSScriptRoot "sign-package.ps1") -PackageDir $stageDir | Out-Host
    if ($LASTEXITCODE -ne 0) { return $LASTEXITCODE }

    $cat = Join-Path $stageDir "WinMaliKmd.cat"
    if (-not (Test-Path $cat)) {
        Write-Host "FATAL: signing completed without producing $cat" -ForegroundColor Red
        return 1
    }
    return 0
}

if ($runMesa) {
    $mesaScript = Join-Path $PSScriptRoot "build-mesa.ps1"
    if (-not (Test-Path $mesaScript)) {
        Write-Host "FATAL: build-mesa.ps1 not found at $mesaScript" -ForegroundColor Red
        exit 4
    }

    # Stage only the native manifest. No Win32 Mesa build or X86 companion
    # staging is part of the ARM64 path.
    if ($Platform -eq "ARM64") {
        Copy-VulkanIcdJsons -nativePlatform "ARM64"
    }

    $code = Invoke-MesaBuild $Platform
    if ($code -ne 0) {
        $overall = $code
        if ($StopOnFirstError) {
            Write-Host "Mesa build failed; stopping (StopOnFirstError)." -ForegroundColor Red
            exit $overall
        }
        Write-Host "Mesa build failed (exit=$code); continuing with KMD." -ForegroundColor Yellow
    }
}

if ($runKmd) {
    $code = Invoke-MsBuild (Join-Path $WinMaliRoot "KMD\WinMaliKmd.vcxproj") "WinMaliKmd"
    if ($code -ne 0 -and $StopOnFirstError) { exit $code }
    if ($code -ne 0) { $overall = $code }

    if ($code -eq 0 -and $SignMode -eq "TestSign") {
        $code = New-TestSignedPackage $Platform
        $script:msbuildResults += [pscustomobject]@{
            Label = "DriverPackage"
            Code  = $code
        }
        if ($code -ne 0 -and $StopOnFirstError) { exit $code }
        if ($code -ne 0) { $overall = $code }
    }
}

Write-Host ""
Write-Host "------------------------------------------------------------" -ForegroundColor DarkGray
foreach ($r in $msbuildResults) {
    $colour = if ($r.Code -eq 0) { 'Green' } else { 'Red' }
    Write-Host ("  {0,-14} {1}" -f $r.Label, $(if ($r.Code -eq 0) { 'OK' } else { "FAILED (exit=$($r.Code))" })) -ForegroundColor $colour
}
Write-Host "------------------------------------------------------------" -ForegroundColor DarkGray

if ($overall -eq 0) {
    Write-Host "BUILD: OK" -ForegroundColor Green
} else {
    Write-Host "BUILD: FAILED (exit=$overall)" -ForegroundColor Red
}
exit $overall
