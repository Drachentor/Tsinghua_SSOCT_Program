[CmdletBinding()]
param(
    [ValidateSet("Both", "Release", "Debug")]
    [string]$Configuration = "Both",

    [string]$BuildRoot = "",
    [string]$ReleaseBuildRoot = "",
    [string]$DebugBuildRoot = "",
    [string]$ExeName = "Tsinghua_SSOCT.exe",
    [string]$QtBin = "",
    [string]$WindeployQt = "",

    [switch]$IncludeCudaRuntime,
    [switch]$Strict
)

$ErrorActionPreference = "Stop"

$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$RepoRoot = (Resolve-Path -LiteralPath (Join-Path $ScriptDir "..")).Path

function Resolve-ExistingFile {
    param([string[]]$Candidates)

    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        $expanded = [Environment]::ExpandEnvironmentVariables($candidate)
        if (Test-Path -LiteralPath $expanded -PathType Leaf) {
            return (Resolve-Path -LiteralPath $expanded).Path
        }
    }
    return $null
}

function Resolve-ExistingDirectory {
    param([string[]]$Candidates)

    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        $expanded = [Environment]::ExpandEnvironmentVariables($candidate)
        if (Test-Path -LiteralPath $expanded -PathType Container) {
            return (Resolve-Path -LiteralPath $expanded).Path
        }
    }
    return $null
}

function Get-ToolFromPath {
    param([string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        return $command.Source
    }
    return $null
}

function Get-QtBinFromMakefile {
    param([string[]]$BuildRoots)

    foreach ($root in $BuildRoots) {
        if ([string]::IsNullOrWhiteSpace($root)) {
            continue
        }
        $makefile = Join-Path $root "Makefile"
        if (!(Test-Path -LiteralPath $makefile -PathType Leaf)) {
            continue
        }

        foreach ($line in Get-Content -LiteralPath $makefile) {
            if ($line -match '^\s*QMAKE\s*=\s*(.+qmake\.exe)\s*$') {
                $qmake = $matches[1].Trim()
                if (Test-Path -LiteralPath $qmake -PathType Leaf) {
                    return (Split-Path -Parent (Resolve-Path -LiteralPath $qmake).Path)
                }
            }
        }
    }
    return $null
}

function Copy-RuntimeFile {
    param(
        [string]$Label,
        [string[]]$Candidates,
        [string]$TargetDir,
        [bool]$Required = $true
    )

    $source = Resolve-ExistingFile -Candidates $Candidates
    if (!$source) {
        $message = "Cannot find $Label. Checked: $($Candidates -join '; ')"
        if ($Required) {
            throw $message
        }
        Write-Warning $message
        return
    }

    $target = Join-Path $TargetDir (Split-Path -Leaf $source)
    Copy-Item -LiteralPath $source -Destination $target -Force
    Write-Host "Copied $Label -> $target"
}

function Copy-LocalRuntimeDlls {
    param(
        [string]$SourceDir,
        [string]$TargetDir
    )

    if (!(Test-Path -LiteralPath $SourceDir -PathType Container)) {
        Write-Warning "Cannot find local runtime DLL directory: $SourceDir"
        return
    }

    Get-ChildItem -LiteralPath $SourceDir -Filter "*.dll" -File | ForEach-Object {
        $target = Join-Path $TargetDir $_.Name
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
        Write-Host "Copied local runtime DLL -> $target"
    }
}

function Invoke-WindeployQt {
    param(
        [string]$ConfigName,
        [string]$ExePath
    )

    $modeArg = if ($ConfigName -eq "Debug") { "--debug" } else { "--release" }
    $arguments = @("--force", "--compiler-runtime", $modeArg, $ExePath)
    Write-Host "Running windeployqt for $ConfigName..."
    & $ResolvedWindeployQt @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "windeployqt failed for $ConfigName with exit code $LASTEXITCODE."
    }
}

$defaultReleaseBuildRoot = Join-Path $RepoRoot "build\QT_5_15_2d-Release"
$defaultDebugBuildRoot = Join-Path $RepoRoot "build\QT_5_15_2d-Debug"
if (![string]::IsNullOrWhiteSpace($BuildRoot)) {
    if ([string]::IsNullOrWhiteSpace($ReleaseBuildRoot)) {
        $ReleaseBuildRoot = $BuildRoot
    }
    if ([string]::IsNullOrWhiteSpace($DebugBuildRoot)) {
        $DebugBuildRoot = $BuildRoot
    }
}
if ([string]::IsNullOrWhiteSpace($ReleaseBuildRoot)) {
    $ReleaseBuildRoot = $defaultReleaseBuildRoot
}
if ([string]::IsNullOrWhiteSpace($DebugBuildRoot)) {
    $DebugBuildRoot = if (Test-Path -LiteralPath $defaultDebugBuildRoot -PathType Container) {
        $defaultDebugBuildRoot
    } else {
        $ReleaseBuildRoot
    }
}

$buildRootsByConfig = @{
    Release = (Resolve-Path -LiteralPath $ReleaseBuildRoot).Path
    Debug = (Resolve-Path -LiteralPath $DebugBuildRoot).Path
}

$qtBinCandidates = @()
if (![string]::IsNullOrWhiteSpace($QtBin)) {
    $qtBinCandidates += $QtBin
}
$qtBinCandidates += Get-QtBinFromMakefile -BuildRoots @($buildRootsByConfig.Release, $buildRootsByConfig.Debug)
$qtBinCandidates += @(
    "D:\QT\5.15.2-Build2\bin",
    "D:\QT\5.15.2-Build-release-only\bin"
)
$ResolvedQtBin = Resolve-ExistingDirectory -Candidates $qtBinCandidates
if (!$ResolvedQtBin) {
    throw "Cannot find a Qt bin directory. Pass -QtBin <path>."
}

$windeployCandidates = @()
if (![string]::IsNullOrWhiteSpace($WindeployQt)) {
    $windeployCandidates += $WindeployQt
}
$windeployCandidates += Join-Path $ResolvedQtBin "windeployqt.exe"
$windeployCandidates += @(
    "D:\QT\Qt5.15.2-src\qttools\bin\windeployqt.exe",
    "D:\QT\5.15.2-Build-release-only\bin\windeployqt.exe",
    (Get-ToolFromPath -Name "windeployqt.exe")
)
$ResolvedWindeployQt = Resolve-ExistingFile -Candidates $windeployCandidates
if (!$ResolvedWindeployQt) {
    throw "Cannot find windeployqt.exe. Pass -WindeployQt <path>."
}

$configs = if ($Configuration -eq "Both") { @("Release", "Debug") } else { @($Configuration) }
$oldPath = $env:PATH
$deployQtBin = Split-Path -Parent $ResolvedWindeployQt
$env:PATH = @($ResolvedQtBin, $deployQtBin, $oldPath) -join ";"

try {
    Write-Host "Release build root: $($buildRootsByConfig.Release)"
    Write-Host "Debug build root: $($buildRootsByConfig.Debug)"
    Write-Host "Qt bin: $ResolvedQtBin"
    Write-Host "windeployqt: $ResolvedWindeployQt"

    foreach ($config in $configs) {
        $configDirName = $config.ToLowerInvariant()
        $buildRootForConfig = $buildRootsByConfig[$config]
        $targetDir = Join-Path $buildRootForConfig $configDirName
        $exePath = Join-Path $targetDir $ExeName

        if (!(Test-Path -LiteralPath $exePath -PathType Leaf)) {
            $message = "$config executable not found: $exePath. Build this configuration first."
            if ($Strict) {
                throw $message
            }
            Write-Warning $message
            continue
        }

        Write-Host ""
        Write-Host "Deploying $config -> $targetDir"
        Invoke-WindeployQt -ConfigName $config -ExePath $exePath

        $opencvDll = if ($config -eq "Debug") { "opencv_world454d.dll" } else { "opencv_world454.dll" }
        Copy-RuntimeFile -Label $opencvDll -TargetDir $targetDir -Candidates @(
            "D:\libsdk\opencv\build\x64\vc15\bin\$opencvDll"
        )
        Copy-LocalRuntimeDlls -SourceDir (Join-Path $RepoRoot "lib") -TargetDir $targetDir
        Copy-RuntimeFile -Label "libiomp5md.dll" -TargetDir $targetDir -Candidates @(
            "D:\libsdk\bin\libiomp5md.dll",
            "C:\Program Files (x86)\Intel\oneAPI\compiler\latest\windows\redist\intel64_win\compiler\libiomp5md.dll",
            "C:\Program Files\Intel\oneAPI\compiler\latest\windows\redist\intel64_win\compiler\libiomp5md.dll"
        )

        if ($IncludeCudaRuntime) {
            $cudaCandidates = @()
            if (![string]::IsNullOrWhiteSpace($env:CUDA_PATH)) {
                $cudaCandidates += @(
                    (Join-Path $env:CUDA_PATH "bin\x64\cufft64_12.dll"),
                    (Join-Path $env:CUDA_PATH "bin\cufft64_12.dll")
                )
            }
            $cudaCandidates += "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2\bin\x64\cufft64_12.dll"
            Copy-RuntimeFile -Label "cufft64_12.dll" -TargetDir $targetDir -Required $false -Candidates $cudaCandidates
        }
    }

    if ($configs -contains "Debug") {
        Write-Warning "Debug deployments require matching MSVC debug runtime DLLs on the target machine. Installing Visual Studio or Build Tools is usually the cleanest way to provide them."
    }

    Write-Host ""
    Write-Host "Deployment finished."
} finally {
    $env:PATH = $oldPath
}
