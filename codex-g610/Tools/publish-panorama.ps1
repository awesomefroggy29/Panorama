<#
.SYNOPSIS
    Publish the current WinMali working tree to panorama/codex-g610.

.DESCRIPTION
    Mirrors files tracked by the rk3588-GPU repository (plus this script while
    it is still untracked) into the codex-g610 subtree, stages only that
    subtree, commits it, and pushes the current Panorama branch.

    The script refuses to run when the Panorama checkout contains changes
    outside codex-g610, so work in claude-g615 or the repository root cannot be
    committed accidentally.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Tools\publish-panorama.ps1

.EXAMPLE
    .\Tools\publish-panorama.ps1 -CommitMessage "Fix preemption fence reporting"

.EXAMPLE
    .\Tools\publish-panorama.ps1 -NoPush
#>

[CmdletBinding()]
param(
    [string]$PanoramaRepo = (Join-Path $env:USERPROFILE "panorama"),
    [string]$CommitMessage = "Sync Codex G610 WinMali changes",
    [switch]$NoPush
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-Git {
    param(
        [Parameter(Mandatory)] [string]$Repository,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [switch]$AllowFailure
    )

    # Keep stderr out of machine-readable stdout. Git can emit benign scanner
    # warnings for inaccessible untracked tool directories while succeeding.
    $output = & git -C $Repository @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "git $($Arguments -join ' ') failed in '$Repository' (exit $exitCode):`n$($output -join "`n")"
    }
    [pscustomobject]@{
        ExitCode = $exitCode
        Output   = @($output)
    }
}

function Get-NormalizedFullPath {
    param([Parameter(Mandatory)] [string]$Path)
    # Git for Windows can emit MSYS-style /c/Users/... paths even when called
    # from Windows PowerShell. Convert that form before using .NET path APIs.
    if ($Path -match '^/([A-Za-z])(?:/(.*))?$') {
        $remainder = $Matches[2] -replace '/', '\'
        $Path = "$($Matches[1]):\$remainder"
    }
    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

$sourceRepo = Get-NormalizedFullPath (Join-Path $PSScriptRoot "..")
if (-not (Test-Path -LiteralPath $PanoramaRepo -PathType Container)) {
    throw "Panorama checkout not found: $PanoramaRepo"
}
$panoramaRoot = Get-NormalizedFullPath (Resolve-Path -LiteralPath $PanoramaRepo).Path
$destination = Get-NormalizedFullPath (Join-Path $panoramaRoot "codex-g610")
if (-not (Test-Path -LiteralPath $destination -PathType Container)) {
    throw "Expected destination folder not found: $destination"
}

$actualTop = (Invoke-Git -Repository $panoramaRoot -Arguments @(
    "rev-parse", "--show-toplevel"
)).Output | Select-Object -First 1
$actualTop = Get-NormalizedFullPath $actualTop
if (-not [string]::Equals($actualTop, $panoramaRoot,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "'$panoramaRoot' is not the root of the Panorama Git checkout (root is '$actualTop')."
}

$origin = (Invoke-Git -Repository $panoramaRoot -Arguments @(
    "remote", "get-url", "origin"
)).Output | Select-Object -First 1
if ($origin -notmatch '(?i)(^|[/:])awesomefroggy29/panorama(?:\.git)?$') {
    throw "Unexpected Panorama origin '$origin'. Refusing to push to the wrong repository."
}

# Refuse to absorb changes belonging to the other developer or repository
# root. Existing codex-g610 changes are in scope and will be replaced/staged.
$panoramaStatus = (Invoke-Git -Repository $panoramaRoot -Arguments @(
    "status", "--porcelain=v1", "--untracked-files=all"
)).Output
$outsideChanges = foreach ($line in $panoramaStatus) {
    if ([string]::IsNullOrWhiteSpace($line) -or $line.Length -lt 4) {
        continue
    }
    $changedPath = $line.Substring(3).Trim('"') -replace '\\', '/'
    if ($changedPath -notlike "codex-g610/*" -and $changedPath -ne "codex-g610") {
        $line
    }
}
if ($outsideChanges) {
    throw "Panorama has changes outside codex-g610. Commit or stash them first:`n$($outsideChanges -join "`n")"
}

$tracked = (Invoke-Git -Repository $sourceRepo -Arguments @(
    "-c", "core.quotepath=false", "ls-files"
)).Output

$sourceFiles = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($relativePath in $tracked) {
    if ([string]::IsNullOrWhiteSpace($relativePath)) {
        continue
    }
    $relativePath = $relativePath -replace '\\', '/'
    $sourcePath = Join-Path $sourceRepo ($relativePath -replace '/', '\')
    if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
        [void]$sourceFiles.Add($relativePath)
    }
}

# Make the publisher self-hosting on its first run even before the user adds
# it to the source repository's index.
$publisherRelativePath = "Tools/publish-panorama.ps1"
[void]$sourceFiles.Add($publisherRelativePath)

$copied = 0
foreach ($relativePath in $sourceFiles) {
    $sourcePath = Join-Path $sourceRepo ($relativePath -replace '/', '\')
    $destinationPath = Join-Path $destination ($relativePath -replace '/', '\')
    $destinationDirectory = Split-Path -Parent $destinationPath
    if (-not (Test-Path -LiteralPath $destinationDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    }
    Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    ++$copied
}

# Remove only files that Git already tracks inside codex-g610 and which no
# longer exist in the source tree. Never recursively delete a directory.
$publishedTracked = (Invoke-Git -Repository $panoramaRoot -Arguments @(
    "-c", "core.quotepath=false", "ls-files", "--", "codex-g610"
)).Output
$removed = 0
foreach ($publishedPath in $publishedTracked) {
    $publishedPath = $publishedPath -replace '\\', '/'
    if (-not $publishedPath.StartsWith("codex-g610/",
            [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    $relativePath = $publishedPath.Substring("codex-g610/".Length)
    if ($sourceFiles.Contains($relativePath)) {
        continue
    }
    $obsoletePath = Get-NormalizedFullPath (Join-Path $panoramaRoot (
        $publishedPath -replace '/', '\'))
    if (-not $obsoletePath.StartsWith($destination + '\',
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove path outside codex-g610: $obsoletePath"
    }
    if (Test-Path -LiteralPath $obsoletePath -PathType Leaf) {
        Remove-Item -LiteralPath $obsoletePath -Force
        ++$removed
    }
}

Invoke-Git -Repository $panoramaRoot -Arguments @(
    "add", "-A", "--", "codex-g610"
) | Out-Null

$check = Invoke-Git -Repository $panoramaRoot -Arguments @(
    "diff", "--cached", "--check", "--", "codex-g610"
) -AllowFailure
if ($check.ExitCode -ne 0) {
    throw "Git whitespace validation failed:`n$($check.Output -join "`n")"
}

$changed = Invoke-Git -Repository $panoramaRoot -Arguments @(
    "diff", "--cached", "--quiet", "--", "codex-g610"
) -AllowFailure
if ($changed.ExitCode -eq 0) {
    Write-Host "Panorama codex-g610 is already current ($copied files checked)."
    exit 0
}
if ($changed.ExitCode -ne 1) {
    throw "Unable to determine whether staged Panorama changes exist."
}

$branch = (Invoke-Git -Repository $panoramaRoot -Arguments @(
    "branch", "--show-current"
)).Output | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($branch)) {
    throw "Panorama is in detached-HEAD state. Check out the intended branch first."
}

Invoke-Git -Repository $panoramaRoot -Arguments @(
    "commit", "-m", $CommitMessage, "--", "codex-g610"
) | ForEach-Object { $_.Output } | Write-Host

if (-not $NoPush) {
    Invoke-Git -Repository $panoramaRoot -Arguments @(
        "push", "origin", $branch
    ) | ForEach-Object { $_.Output } | Write-Host
}

Write-Host "Published $copied files to codex-g610 (removed $removed obsolete files)."
Write-Host "Branch: $branch"
Write-Host "Commit: $((Invoke-Git -Repository $panoramaRoot -Arguments @('rev-parse', 'HEAD')).Output[0])"
if ($NoPush) {
    Write-Host "Push skipped (-NoPush)."
} else {
    Write-Host "Pushed to: $origin"
}
