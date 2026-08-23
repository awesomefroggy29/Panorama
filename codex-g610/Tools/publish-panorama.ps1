<#
.SYNOPSIS
    Publish either the WinMali KMD tree or mesa-modern UMD tree to Panorama.

.DESCRIPTION
    -KMD mirrors files tracked by rk3588-GPU (plus this script while it is
    still untracked) into panorama/codex-g610.

    -UMD mirrors files tracked by the mesa-modern checkout into
    panorama/codex-g610/mesa-modern, creating that folder on first publish.

    Exactly one mode is required. Each mode stages and commits only its own
    subtree, so publishing KMD cannot delete or absorb Mesa work and publishing
    UMD cannot absorb pending KMD work.

    The script refuses to run when the Panorama checkout contains changes
    outside the selected layer, so work in claude-g615, the repository root,
    or the other Codex layer cannot be committed accidentally.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Tools\publish-panorama.ps1 -KMD

.EXAMPLE
    .\Tools\publish-panorama.ps1 -KMD -CommitMessage "Fix preemption fence reporting"

.EXAMPLE
    .\Tools\publish-panorama.ps1 -UMD -CommitMessage "Sync Mesa render changes"

.EXAMPLE
    .\Tools\publish-panorama.ps1 -UMD -NoPush
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory, ParameterSetName = "KMD")]
    [switch]$KMD,

    [Parameter(Mandatory, ParameterSetName = "UMD")]
    [switch]$UMD,

    [string]$PanoramaRepo = (Join-Path $env:USERPROFILE "panorama"),
    [string]$MesaRepo = (Join-Path $env:USERPROFILE "mesa-modern\mesa"),
    [string]$CommitMessage,
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

function Get-GitIndexBlobMap {
    param(
        [Parameter(Mandatory)] [string]$Repository,
        [string]$Pathspec,
        [string]$StripPrefix = ""
    )

    $arguments = @("-c", "core.quotepath=false", "ls-files", "-s")
    if (-not [string]::IsNullOrWhiteSpace($Pathspec)) {
        $arguments += @("--", $Pathspec)
    }

    $map = [System.Collections.Generic.Dictionary[string,string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    $prefix = $StripPrefix.TrimEnd('/')
    if ($prefix.Length -ne 0) {
        $prefix += "/"
    }

    foreach ($line in (Invoke-Git -Repository $Repository -Arguments $arguments).Output) {
        # Mode, blob object ID, stage, then a tab and the path. Only stage 0 is
        # usable; a nonzero stage means the repository has an unresolved merge.
        if ($line -notmatch '^\d+\s+([0-9a-fA-F]+)\s+0\t(.+)$') {
            continue
        }
        $blobId = $Matches[1]
        $path = $Matches[2] -replace '\\', '/'
        if ($prefix.Length -ne 0) {
            if (-not $path.StartsWith($prefix,
                    [System.StringComparison]::OrdinalIgnoreCase)) {
                continue
            }
            $path = $path.Substring($prefix.Length)
        }
        $map[$path] = $blobId
    }
    # Prevent PowerShell from unrolling the generic dictionary into key/value
    # entries as it crosses the function pipeline boundary.
    return ,$map
}

function Get-GitWorkingBlobHash {
    param(
        [Parameter(Mandatory)] [string]$Repository,
        [Parameter(Mandatory)] [string]$RelativePath
    )

    $result = Invoke-Git -Repository $Repository -Arguments @(
        "hash-object", "--", ($RelativePath -replace '\\', '/')
    )
    return $result.Output | Select-Object -First 1
}

function Test-WinMaliUmdSourcePath {
    param([Parameter(Mandatory)] [string]$RelativePath)

    $path = $RelativePath.Replace('\', '/')

    # Root files required to configure and redistribute this Mesa subset.
    if ($path -in @(
        ".gitattributes", ".gitignore", "README.rst", "VERSION",
        "meson.build", "meson.options"
    )) {
        return $true
    }

    # This is the source closure used by the configured WinMali targets:
    # OpenGL/WGL, the D3D10/11 Gallium UMD, panfrost/panthor, panvk, and the
    # native shader-generation tools needed by ARM64 cross-builds. Keep broad
    # shared-library directories whole so headers and generator inputs are not
    # accidentally omitted merely because they are not compiler translation
    # units themselves.
    $prefixes = @(
        "bin/",
        "build-support/",
        "include/",
        "licenses/",
        "subprojects/",
        "src/c11/",
        "src/android_stub/",
        "src/compiler/",
        "src/egl/",
        "src/getopt/",
        "src/gtest/",
        "src/loader/",
        "src/mesa/",
        "src/panfrost/",
        "src/tool/",
        "src/util/",
        "src/vulkan/",
        "src/gallium/auxiliary/",
        "src/gallium/include/",
        "src/gallium/drivers/panfrost/",
        "src/gallium/frontends/d3d10umd/",
        "src/gallium/frontends/wgl/",
        "src/gallium/targets/d3d10rk3588/",
        "src/gallium/targets/d3d10umd/",
        "src/gallium/targets/libgl-gdi/",
        "src/gallium/targets/wgl/",
        "src/gallium/winsys/panfrost/",
        "src/gallium/winsys/sw/"
    )
    foreach ($prefix in $prefixes) {
        if ($path.StartsWith($prefix,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }

    # Meson entry points above the selected leaf directories.
    return $path -in @(
        "src/meson.build",
        "src/gallium/meson.build"
    )
}

$mode = if ($KMD) { "KMD" } else { "UMD" }
if ([string]::IsNullOrWhiteSpace($CommitMessage)) {
    $CommitMessage = if ($mode -eq "KMD") {
        "Sync Codex G610 WinMali KMD changes"
    } else {
        "Sync Codex G610 mesa-modern UMD changes"
    }
}

$sourceRepo = if ($mode -eq "KMD") {
    Get-NormalizedFullPath (Join-Path $PSScriptRoot "..")
} else {
    if (-not (Test-Path -LiteralPath $MesaRepo -PathType Container)) {
        throw "mesa-modern checkout not found: $MesaRepo"
    }
    Get-NormalizedFullPath (Resolve-Path -LiteralPath $MesaRepo).Path
}
if (-not (Test-Path -LiteralPath $PanoramaRepo -PathType Container)) {
    throw "Panorama checkout not found: $PanoramaRepo"
}
$panoramaRoot = Get-NormalizedFullPath (Resolve-Path -LiteralPath $PanoramaRepo).Path
$codexRoot = Get-NormalizedFullPath (Join-Path $panoramaRoot "codex-g610")
if (-not (Test-Path -LiteralPath $codexRoot -PathType Container)) {
    throw "Expected destination folder not found: $codexRoot"
}
$destinationRelative = if ($mode -eq "KMD") {
    "codex-g610"
} else {
    "codex-g610/mesa-modern"
}
$destination = Get-NormalizedFullPath (Join-Path $panoramaRoot (
    $destinationRelative -replace '/', '\'))

$sourceTop = (Invoke-Git -Repository $sourceRepo -Arguments @(
    "rev-parse", "--show-toplevel"
)).Output | Select-Object -First 1
$sourceTop = Get-NormalizedFullPath $sourceTop
if (-not [string]::Equals($sourceTop, $sourceRepo,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "'$sourceRepo' is not the root of the $mode source checkout (root is '$sourceTop')."
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

$branch = (Invoke-Git -Repository $panoramaRoot -Arguments @(
    "branch", "--show-current"
)).Output | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($branch)) {
    throw "Panorama is in detached-HEAD state. Check out the intended branch first."
}

# Refuse to absorb changes belonging to the other developer, repository root,
# or the other Codex layer. Existing changes are allowed only in the selected
# mode's publish scope.
$panoramaStatus = (Invoke-Git -Repository $panoramaRoot -Arguments @(
    "status", "--porcelain=v1", "--untracked-files=all"
)).Output
$outsideChanges = foreach ($line in $panoramaStatus) {
    if ([string]::IsNullOrWhiteSpace($line) -or $line.Length -lt 4) {
        continue
    }
    $changedPath = $line.Substring(3).Trim('"') -replace '\\', '/'
    $inScope = if ($mode -eq "UMD") {
        $changedPath -eq "codex-g610/mesa-modern" -or
        $changedPath.StartsWith("codex-g610/mesa-modern/",
            [System.StringComparison]::OrdinalIgnoreCase)
    } else {
        ($changedPath -eq "codex-g610" -or
         $changedPath.StartsWith("codex-g610/",
            [System.StringComparison]::OrdinalIgnoreCase)) -and
        -not ($changedPath -eq "codex-g610/mesa-modern" -or
              $changedPath.StartsWith("codex-g610/mesa-modern/",
                [System.StringComparison]::OrdinalIgnoreCase))
    }
    if (-not $inScope) {
        $line
    }
}
if ($outsideChanges) {
    throw "Panorama has changes outside the selected $mode publish scope. Commit or stash them first:`n$($outsideChanges -join "`n")"
}

Write-Host "Publishing $mode sources"
Write-Host "  Source      : $sourceRepo"
Write-Host "  Destination : $destination"
Write-Host "Enumerating tracked files..."

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
    if ($mode -eq "UMD" -and
        -not (Test-WinMaliUmdSourcePath -RelativePath $relativePath)) {
        continue
    }
    $sourcePath = Join-Path $sourceRepo ($relativePath -replace '/', '\')
    if (Test-Path -LiteralPath $sourcePath -PathType Leaf) {
        [void]$sourceFiles.Add($relativePath)
    }
}

# Make the KMD publisher self-hosting on its first run even before the user
# adds it to the rk3588-GPU source repository's index. It is not part of Mesa.
if ($mode -eq "KMD") {
    $publisherRelativePath = "Tools/publish-panorama.ps1"
    [void]$sourceFiles.Add($publisherRelativePath)
}

Write-Host "Found $($sourceFiles.Count) tracked source files."

if (-not (Test-Path -LiteralPath $destination -PathType Container)) {
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
}

$sourceObjectFormat = (Invoke-Git -Repository $sourceRepo -Arguments @(
    "rev-parse", "--show-object-format"
)).Output | Select-Object -First 1
$panoramaObjectFormat = (Invoke-Git -Repository $panoramaRoot -Arguments @(
    "rev-parse", "--show-object-format"
)).Output | Select-Object -First 1
if ($sourceObjectFormat -ne $panoramaObjectFormat) {
    throw "Source uses Git object format '$sourceObjectFormat' but Panorama uses '$panoramaObjectFormat'. Incremental content comparison is not possible."
}

# Git has already hashed every clean tracked file into each repository's
# index. Reuse those blob IDs rather than copying and SHA-256 hashing the
# entire Mesa checkout on every publish. Only files modified in either
# working tree need to be hashed again from disk.
Write-Host "Finding changed files from Git blob IDs..."
$sourceBlobs = Get-GitIndexBlobMap -Repository $sourceRepo
$destinationBlobs = Get-GitIndexBlobMap -Repository $panoramaRoot `
    -Pathspec $destinationRelative -StripPrefix $destinationRelative

$sourceDirty = (Invoke-Git -Repository $sourceRepo -Arguments @(
    "-c", "core.quotepath=false", "diff", "--name-only", "--no-renames",
    "HEAD", "--"
)).Output
foreach ($relativePath in $sourceDirty) {
    $relativePath = $relativePath -replace '\\', '/'
    if ($sourceFiles.Contains($relativePath)) {
        $sourceBlobs[$relativePath] = Get-GitWorkingBlobHash `
            -Repository $sourceRepo -RelativePath $relativePath
    }
}

# The self-hosted KMD publisher can be untracked on its first run. More
# generally, hash any selected source file absent from the source index.
foreach ($relativePath in $sourceFiles) {
    if (-not $sourceBlobs.ContainsKey($relativePath)) {
        $sourceBlobs[$relativePath] = Get-GitWorkingBlobHash `
            -Repository $sourceRepo -RelativePath $relativePath
    }
}

$destinationDirty = @(
    (Invoke-Git -Repository $panoramaRoot -Arguments @(
        "-c", "core.quotepath=false", "diff", "--name-only", "--no-renames",
        "HEAD", "--", $destinationRelative
    )).Output
    (Invoke-Git -Repository $panoramaRoot -Arguments @(
        "-c", "core.quotepath=false", "ls-files", "--others",
        "--exclude-standard", "--", $destinationRelative
    )).Output
)
$destinationPrefix = $destinationRelative.TrimEnd('/') + "/"
foreach ($publishedPath in $destinationDirty) {
    $publishedPath = $publishedPath -replace '\\', '/'
    if (-not $publishedPath.StartsWith($destinationPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    $relativePath = $publishedPath.Substring($destinationPrefix.Length)
    $destinationPath = Join-Path $destination ($relativePath -replace '/', '\')
    if (Test-Path -LiteralPath $destinationPath -PathType Leaf) {
        $destinationBlobs[$relativePath] = Get-GitWorkingBlobHash `
            -Repository $panoramaRoot -RelativePath $publishedPath
    }
    else {
        [void]$destinationBlobs.Remove($relativePath)
    }
}

$filesToCopy = [System.Collections.Generic.List[string]]::new()
foreach ($relativePath in $sourceFiles) {
    $destinationPath = Join-Path $destination ($relativePath -replace '/', '\')
    # A source-tracked file can be ignored by Mesa's own .gitignore after it
    # is nested inside Panorama. If it already exists from an earlier mirror
    # run but is not yet in Panorama's index, hash that one file directly.
    if ((Test-Path -LiteralPath $destinationPath -PathType Leaf) -and
        -not $destinationBlobs.ContainsKey($relativePath)) {
        $publishedPath = $destinationPrefix + $relativePath
        $destinationBlobs[$relativePath] = Get-GitWorkingBlobHash `
            -Repository $panoramaRoot -RelativePath $publishedPath
    }
    if (-not (Test-Path -LiteralPath $destinationPath -PathType Leaf) -or
        -not $destinationBlobs.ContainsKey($relativePath) -or
        $sourceBlobs[$relativePath] -ne $destinationBlobs[$relativePath]) {
        $filesToCopy.Add($relativePath)
    }
}

$copied = 0
Write-Host "Copying $($filesToCopy.Count) changed file(s)..."
foreach ($relativePath in $filesToCopy) {
    $sourcePath = Join-Path $sourceRepo ($relativePath -replace '/', '\')
    $destinationPath = Join-Path $destination ($relativePath -replace '/', '\')
    $destinationDirectory = Split-Path -Parent $destinationPath
    if (-not (Test-Path -LiteralPath $destinationDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    }
    Copy-Item -LiteralPath $sourcePath -Destination $destinationPath -Force
    ++$copied
    if (($copied % 100) -eq 0 -or $copied -eq $filesToCopy.Count) {
        $percent = [int](($copied * 100) / [Math]::Max(1, $filesToCopy.Count))
        Write-Progress -Activity "Publishing $mode sources" `
            -Status "Copying file $copied of $($filesToCopy.Count)" `
            -PercentComplete $percent
    }
}
Write-Progress -Activity "Publishing $mode sources" -Completed

# Verify files actually copied during this run. Unchanged files were already
# proven equal by their Git blob IDs, so re-reading all 12,000+ files would add
# minutes without improving the content comparison.
$mismatches = [System.Collections.Generic.List[string]]::new()
$verified = 0
Write-Host "Verifying $($filesToCopy.Count) copied file(s) with SHA-256..."
foreach ($relativePath in $filesToCopy) {
    ++$verified
    if (($verified % 100) -eq 0 -or $verified -eq $filesToCopy.Count) {
        $percent = [int](($verified * 100) / [Math]::Max(1, $filesToCopy.Count))
        Write-Progress -Activity "Verifying $mode sources" `
            -Status "Hashing file $verified of $($filesToCopy.Count)" `
            -PercentComplete $percent
    }
    $sourcePath = Join-Path $sourceRepo ($relativePath -replace '/', '\')
    $destinationPath = Join-Path $destination ($relativePath -replace '/', '\')
    if (-not (Test-Path -LiteralPath $destinationPath -PathType Leaf)) {
        $mismatches.Add("$relativePath (missing)")
        continue
    }
    $sourceHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourcePath).Hash
    $destinationHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $destinationPath).Hash
    if ($sourceHash -ne $destinationHash) {
        $mismatches.Add("$relativePath (hash mismatch)")
    }
}
Write-Progress -Activity "Verifying $mode sources" -Completed
if ($mismatches.Count -ne 0) {
    throw "Mirror verification failed for $($mismatches.Count) file(s):`n$($mismatches -join "`n")"
}

# Remove only files that Git already tracks inside the selected layer and
# which no longer exist in that source tree. Never recursively delete a
# directory. In KMD mode, mesa-modern is explicitly outside the layer.
Write-Host "Checking for stale published files..."
$publishedTracked = (Invoke-Git -Repository $panoramaRoot -Arguments @(
    "-c", "core.quotepath=false", "ls-files", "--", $destinationRelative
)).Output
$removed = 0
foreach ($publishedPath in $publishedTracked) {
    $publishedPath = $publishedPath -replace '\\', '/'
    if ($mode -eq "KMD" -and
        $publishedPath.StartsWith("codex-g610/mesa-modern/",
            [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    $destinationPrefix = $destinationRelative.TrimEnd('/') + "/"
    if (-not $publishedPath.StartsWith($destinationPrefix,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    $relativePath = $publishedPath.Substring($destinationPrefix.Length)
    # Existing files outside the WinMali build closure may have been imported
    # by an older full-tree publisher. Leave those snapshots untouched here;
    # this scoped publisher neither scans nor stages their deletion.
    if ($mode -eq "UMD" -and
        -not (Test-WinMaliUmdSourcePath -RelativePath $relativePath)) {
        continue
    }
    if ($sourceFiles.Contains($relativePath)) {
        continue
    }
    $obsoletePath = Get-NormalizedFullPath (Join-Path $panoramaRoot (
        $publishedPath -replace '/', '\'))
    if (-not $obsoletePath.StartsWith($destination + '\',
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove path outside ${destinationRelative}: $obsoletePath"
    }
    if (Test-Path -LiteralPath $obsoletePath -PathType Leaf) {
        Remove-Item -LiteralPath $obsoletePath -Force
        ++$removed
    }
}

$gitPathspec = if ($mode -eq "KMD") {
    @("codex-g610", ":(exclude)codex-g610/mesa-modern/**")
} else {
    # Stage/commit only the same WinMali build closure selected above. Older
    # full-tree imports may remain in Panorama, but unrelated Mesa drivers,
    # documentation, and CI files must not hitch a ride on a UMD publish.
    @(
        ".gitattributes", ".gitignore", "README.rst", "VERSION",
        "meson.build", "meson.options",
        "bin", "build-support", "include", "licenses", "subprojects",
        "src/meson.build",
        "src/android_stub", "src/c11", "src/compiler", "src/egl",
        "src/getopt", "src/gtest",
        "src/loader", "src/mesa", "src/panfrost", "src/util", "src/vulkan",
        "src/tool",
        "src/gallium/meson.build",
        "src/gallium/auxiliary", "src/gallium/include",
        "src/gallium/drivers/panfrost",
        "src/gallium/frontends/d3d10umd", "src/gallium/frontends/wgl",
        "src/gallium/targets/d3d10rk3588",
        "src/gallium/targets/d3d10umd", "src/gallium/targets/libgl-gdi",
        "src/gallium/targets/wgl",
        "src/gallium/winsys/panfrost", "src/gallium/winsys/sw"
    ) | ForEach-Object { "$destinationRelative/$_" }
}

Write-Host "Staging $mode changes..."
$stageArguments = if ($mode -eq "UMD") {
    # Mesa tracks some files that its own ignore rules also match. Once Mesa
    # is nested in Panorama, a normal add silently omits those source-tracked
    # files. Force is safe here because this dedicated destination is a mirror
    # and the script has already limited its scope to mesa-modern.
    @("add", "-f", "-A", "--") + $gitPathspec
}
else {
    @("add", "-A", "--") + $gitPathspec
}
Invoke-Git -Repository $panoramaRoot -Arguments $stageArguments | Out-Null

Write-Host "Checking source changes..."
if ($mode -eq "UMD") {
    # mesa-modern is imported beneath a new directory in Panorama. Running
    # diff --cached --check there would report every historical whitespace
    # issue in the entire Mesa tree as a newly introduced problem. Instead,
    # ask Mesa's own repository to check only changes relative to its HEAD.
    $check = Invoke-Git -Repository $sourceRepo -Arguments @(
        "diff", "--check", "HEAD", "--"
    ) -AllowFailure
}
else {
    $check = Invoke-Git -Repository $panoramaRoot -Arguments (@(
        "diff", "--cached", "--check", "--"
    ) + $gitPathspec) -AllowFailure
}
if ($check.ExitCode -ne 0) {
    throw "Git whitespace validation failed for $mode source changes:`n$($check.Output -join "`n")"
}

$changed = Invoke-Git -Repository $panoramaRoot -Arguments (@(
    "diff", "--cached", "--quiet", "--"
) + $gitPathspec) -AllowFailure
if ($changed.ExitCode -eq 0) {
    Write-Host "Panorama $destinationRelative files are already current ($copied changed files copied)."
    Write-Host "Checking for committed changes that still need to be pushed..."
}
elseif ($changed.ExitCode -ne 1) {
    throw "Unable to determine whether staged Panorama changes exist."
}
else {
    Invoke-Git -Repository $panoramaRoot -Arguments (@(
        "commit", "-m", $CommitMessage, "--"
    ) + $gitPathspec) | ForEach-Object { $_.Output } | Write-Host
}

if (-not $NoPush) {
    # A previous push can fail when another developer advances main. Fetch and
    # replay only our local commits on top of the new remote tip, preserving a
    # linear history and avoiding force-pushes. This also lets a later run push
    # a commit created by an earlier run even when the mirrored files match.
    Invoke-Git -Repository $panoramaRoot -Arguments @(
        "fetch", "origin", $branch
    ) | ForEach-Object { $_.Output } | Write-Host

    $remoteBranch = "origin/$branch"
    $remoteIsAncestor = Invoke-Git -Repository $panoramaRoot -Arguments @(
        "merge-base", "--is-ancestor", $remoteBranch, "HEAD"
    ) -AllowFailure
    if ($remoteIsAncestor.ExitCode -eq 1) {
        Write-Host "Remote $branch advanced; rebasing local commits onto $remoteBranch..."
        Invoke-Git -Repository $panoramaRoot -Arguments @(
            "rebase", $remoteBranch
        ) | ForEach-Object { $_.Output } | Write-Host
    }
    elseif ($remoteIsAncestor.ExitCode -ne 0) {
        throw "Unable to compare local HEAD with $remoteBranch."
    }

    Invoke-Git -Repository $panoramaRoot -Arguments @(
        "push", "origin", $branch
    ) | ForEach-Object { $_.Output } | Write-Host
}

Write-Host "Published $copied $mode files to $destinationRelative (removed $removed obsolete files)."
Write-Host "Verified: all copied files match the source checkout."
Write-Host "Branch: $branch"
Write-Host "Commit: $((Invoke-Git -Repository $panoramaRoot -Arguments @('rev-parse', 'HEAD')).Output[0])"
if ($NoPush) {
    Write-Host "Push skipped (-NoPush)."
} else {
    Write-Host "Pushed to: $origin"
}
