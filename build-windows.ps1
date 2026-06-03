# build-windows.ps1 - Windows build helper for obs-graphics-studio-pro.
# Validates prerequisites, configures CMake, builds, and installs the plugin.

param(
    [string]$BuildDir,
    [string]$VcpkgDir,
    [string]$ObsSdkDir,
    [string]$InstallRoot,
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Architecture = "x64",
    [switch]$SkipInstall
)

$ErrorActionPreference = "Stop"

# Paths
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $ScriptDir "build"
}
if ([string]::IsNullOrWhiteSpace($VcpkgDir)) {
    $VcpkgDir = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\vcpkg" }
}
if ([string]::IsNullOrWhiteSpace($ObsSdkDir) -and $env:OBS_SDK_DIR) {
    $ObsSdkDir = $env:OBS_SDK_DIR
}
if ([string]::IsNullOrWhiteSpace($ObsSdkDir) -and $env:OBS_STUDIO_DIR) {
    $ObsSdkDir = $env:OBS_STUDIO_DIR
}
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    if ($env:OBS_PLUGINS_PATH) {
        $InstallRoot = $env:OBS_PLUGINS_PATH
    } elseif ($env:ProgramData) {
        $InstallRoot = Join-Path $env:ProgramData "obs-studio\plugins"
    } else {
        $InstallRoot = Join-Path $env:APPDATA "obs-studio\plugins"
    }
}

$PluginName = "obs-graphics-studio-pro"
$VcpkgToolchain = Join-Path $VcpkgDir "scripts\buildsystems\vcpkg.cmake"
$ObsArchDir = if ($Architecture -eq "Win32" -or $Architecture -eq "x86") { "32bit" } else { "64bit" }
$PluginDllName = "$PluginName.dll"
$ObsPluginRoot = Join-Path $InstallRoot $PluginName
$ObsPluginBin = Join-Path $ObsPluginRoot "bin\$ObsArchDir"
$ObsPluginData = Join-Path $ObsPluginRoot "data\locale"

Write-Host "=== Starting OBS Graphics Studio Pro build process ==="


# Guard against accidental duplicate out-of-class bodies in large UI
# translation units. MSVC reports these late during compilation, so fail early
# with the exact repeated definitions that have previously broken Windows builds.
function Assert-UniqueSourceDefinition {
    param(
        [string]$File,
        [string[]]$Definitions
    )

    if (-not (Test-Path $File)) {
        Write-Error "Source file not found: $File"
        exit 1
    }

    $Text = Get-Content -Raw -Path $File
    foreach ($Definition in $Definitions) {
        $Count = ([regex]::Matches($Text, [regex]::Escape($Definition))).Count
        if ($Count -gt 1) {
            Write-Error "Duplicate definition detected in ${File}: '$Definition' appears $Count times. Remove the duplicate body before building."
            exit 1
        }
    }
}

$TitleEditorSource = Join-Path $ScriptDir "src\title-editor.cpp"
Assert-UniqueSourceDefinition -File $TitleEditorSource -Definitions @(
    "void TitleEditor::keyPressEvent(",
    "void CanvasPreview::set_safe_guides_visible(",
    "void CanvasPreview::refresh_preview(",
    "std::shared_ptr<Layer> CanvasPreview::selected_layer(",
    "QRectF CanvasPreview::layer_local_rect(",
    "double CanvasPreview::view_scale(",
    "QPointF CanvasPreview::view_origin(",
    "QPointF CanvasPreview::view_to_canvas(",
    "QPointF CanvasPreview::canvas_to_view(",
    "QPointF CanvasPreview::canvas_to_layer(",
    "QPointF CanvasPreview::layer_to_canvas(",
    "CanvasPreview::DragMode CanvasPreview::hit_test_selected(",
    "void CanvasPreview::apply_drag(",
    "void TimelineWidget::contextMenuEvent(",
    "void TimelineWidget::wheelEvent(",
    "TitlePropertiesPanel::TitlePropertiesPanel(",
    "void TitlePropertiesPanel::set_title(",
    "void TitlePropertiesPanel::load_values("
)

$TitleDockSource = Join-Path $ScriptDir "src\title-dock.cpp"
Assert-UniqueSourceDefinition -File $TitleDockSource -Definitions @(
    "void TitleDock::select_title(",
    "std::shared_ptr<Title> TitleDock::create_template_title(",
    "void TitleDock::create_title_from_template("
)

# 1. Verify CMake and Visual Studio
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error "CMake not found. Please install CMake and add it to your PATH."
    exit 1
}

# 2. Verify vcpkg toolchain
if (-not (Test-Path $VcpkgToolchain)) {
    Write-Error "vcpkg toolchain not found at $VcpkgToolchain. Pass -VcpkgDir or set VCPKG_ROOT."
    exit 1
}

# 3. Detect OBS SDK/build dependencies without machine-specific paths.
function Test-ObsSdkDir {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path $Path)) {
        return $false
    }

    $HasObsLib = (Test-Path (Join-Path $Path "lib\obs.lib")) -or
                 (Test-Path (Join-Path $Path "lib\obs\obs.lib"))
    $HasObsFrontendLib = (Test-Path (Join-Path $Path "lib\obs-frontend-api.lib")) -or
                         (Test-Path (Join-Path $Path "lib\obs\obs-frontend-api.lib"))
    $HasObsHeader = (Test-Path (Join-Path $Path "include\obs.h")) -or
                    (Test-Path (Join-Path $Path "include\obs\obs.h"))
    return ($HasObsLib -and $HasObsFrontendLib -and $HasObsHeader)
}

if (-not (Test-ObsSdkDir $ObsSdkDir)) {
    $CandidateRoots = @(
        (Join-Path $env:USERPROFILE "Desktop\obs-build-dependencies"),
        (Join-Path $env:USERPROFILE "Downloads\obs-build-dependencies"),
        (Join-Path $ScriptDir "obs-build-dependencies"),
        (Join-Path $env:ProgramFiles "obs-studio")
    )

    if ($env:ProgramW6432) {
        $CandidateRoots += (Join-Path $env:ProgramW6432 "obs-studio")
    }

    foreach ($Root in $CandidateRoots) {
        if ([string]::IsNullOrWhiteSpace($Root) -or -not (Test-Path $Root)) {
            continue
        }

        $Candidates = @($Root)
        $Candidates += @(Get-ChildItem -Path $Root -Directory -Filter "plugin-deps-*" -ErrorAction SilentlyContinue |
                         Sort-Object Name -Descending |
                         ForEach-Object { $_.FullName })

        foreach ($Candidate in $Candidates) {
            if (Test-ObsSdkDir $Candidate) {
                $ObsSdkDir = $Candidate
                break
            }
        }

        if (Test-ObsSdkDir $ObsSdkDir) {
            break
        }
    }
}

if (-not (Test-ObsSdkDir $ObsSdkDir)) {
    Write-Error "Could not locate an OBS SDK/install tree. Pass -ObsSdkDir or set OBS_SDK_DIR/OBS_STUDIO_DIR."
    exit 1
}
Write-Host "Found OBS SDK: $ObsSdkDir"

# 4. Configure CMake
Write-Host "`n=== Configuring CMake ==="
$CmakeArgs = @(
    "-B", $BuildDir,
    "-G", $Generator,
    "-A", $Architecture,
    "-DCMAKE_TOOLCHAIN_FILE=$($VcpkgToolchain.Replace('\', '/'))",
    "-DOBS_SDK_DIR=$($ObsSdkDir.Replace('\', '/'))"
)
& cmake @CmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed."
    exit 1
}

# 5. Build the Plugin
Write-Host "`n=== Building OBS Graphics Studio Pro ==="
& cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed."
    exit 1
}

if ($SkipInstall) {
    Write-Host "`n=== Build complete; skipping OBS install because -SkipInstall was set. ==="
    exit 0
}

# 6. Copy build DLL and Locale to OBS plugins directory
Write-Host "`n=== Installing Plugin to OBS ==="
Write-Host "Install root: $InstallRoot"
New-Item -ItemType Directory -Force -Path $ObsPluginBin | Out-Null
New-Item -ItemType Directory -Force -Path $ObsPluginData | Out-Null

$StagedPluginRoot = Join-Path $BuildDir $PluginName
$BuiltDllCandidates = @(
    (Join-Path $StagedPluginRoot "bin\$ObsArchDir\$PluginDllName"),
    (Join-Path $StagedPluginRoot "bin\$ObsArchDir\Release\$PluginDllName"),
    (Join-Path $StagedPluginRoot "bin\$ObsArchDir\RelWithDebInfo\$PluginDllName"),
    (Join-Path $StagedPluginRoot "bin\$ObsArchDir\Debug\$PluginDllName"),
    (Join-Path $BuildDir "obs-plugins\Release\$PluginDllName"),
    (Join-Path $BuildDir "obs-plugins\$PluginDllName"),
    (Join-Path $BuildDir "obs-plugins\$ObsArchDir\$PluginDllName"),
    (Join-Path $BuildDir "Release\$PluginDllName"),
    (Join-Path $BuildDir "RelWithDebInfo\$PluginDllName"),
    (Join-Path $BuildDir "Debug\$PluginDllName")
)
$BuiltDll = $BuiltDllCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $BuiltDll) {
    $RecursiveMatch = Get-ChildItem -Path $BuildDir -Filter $PluginDllName -Recurse -File -ErrorAction SilentlyContinue |
        Sort-Object @{ Expression = { if ($_.FullName -like "*\$PluginName\bin\$ObsArchDir*") { 0 } else { 1 } } }, FullName |
        Select-Object -First 1
    if ($RecursiveMatch) {
        $BuiltDll = $RecursiveMatch.FullName
    }
}

if (-not $BuiltDll) {
    Write-Error "Could not find built $PluginDllName. Checked known locations: $($BuiltDllCandidates -join ', '). Also searched recursively under $BuildDir."
    exit 1
}

Copy-Item -Force $BuiltDll $ObsPluginBin
Write-Host "Copied plugin DLL from: $BuiltDll"
Write-Host "Copied plugin DLL to: $ObsPluginBin"

$StagedData = Join-Path $StagedPluginRoot "data"
if (Test-Path $StagedData) {
    Copy-Item -Force -Recurse (Join-Path $StagedData "*") (Join-Path $ObsPluginRoot "data")
    Write-Host "Copied staged plugin data to: $(Join-Path $ObsPluginRoot 'data')"
} else {
    $LocaleFile = Join-Path $ScriptDir "data\locale\en-US.ini"
    if (Test-Path $LocaleFile) {
        Copy-Item -Force $LocaleFile $ObsPluginData
        Write-Host "Copied en-US.ini to: $ObsPluginData"
    }
}

# 7. Copy runtime DLL dependencies next to the plugin binary.
# A plugin can compile and still fail to load in OBS if Qt/Cairo/Pango DLLs are
# not beside obs-graphics-studio-pro.dll, so copy every vcpkg runtime DLL rather than trying
# to maintain a fragile hand-written dependency list.
Write-Host "`n=== Copying runtime DLL dependencies ==="
$RuntimeDllDirs = @()
$VcpkgTriplets = @($Architecture.ToLower())
if ($Architecture -eq "x64") {
    $VcpkgTriplets += "x64-windows"
} elseif ($Architecture -eq "Win32") {
    $VcpkgTriplets += "x86-windows"
}

foreach ($Triplet in ($VcpkgTriplets | Select-Object -Unique)) {
    $CandidateBin = Join-Path $VcpkgDir "installed\$Triplet\bin"
    if (Test-Path $CandidateBin) {
        $RuntimeDllDirs += $CandidateBin
    }
}

$CopiedCount = 0
foreach ($RuntimeDllDir in ($RuntimeDllDirs | Select-Object -Unique)) {
    Write-Host "Copying DLLs from: $RuntimeDllDir"
    $RuntimeDlls = Get-ChildItem -Path $RuntimeDllDir -Filter "*.dll" -File -ErrorAction SilentlyContinue
    foreach ($Dll in $RuntimeDlls) {
        Copy-Item -Force $Dll.FullName $ObsPluginBin
        $CopiedCount++
    }
}

if ($CopiedCount -eq 0) {
    Write-Warning "No vcpkg runtime DLLs were copied. If OBS says obs-graphics-studio-pro failed to load, check for missing Qt/Cairo/Pango DLLs in $ObsPluginBin."
} else {
    Write-Host "Copied $CopiedCount runtime DLL dependencies."
}

$ExpectedDlls = @(
    $PluginDllName,
    "cairo.dll",
    "pango-1.0.dll",
    "pangocairo-1.0.dll"
)
$MissingExpectedDlls = @()
foreach ($Dll in $ExpectedDlls) {
    if (-not (Test-Path (Join-Path $ObsPluginBin $Dll))) {
        $MissingExpectedDlls += $Dll
    }
}
if ($MissingExpectedDlls.Count -gt 0) {
    Write-Warning "The install folder is missing expected DLL(s): $($MissingExpectedDlls -join ', '). OBS may report that obs-graphics-studio-pro failed to load."
}

Write-Host "`nInstalled OBS plugin layout:"
Write-Host "  $ObsPluginRoot"
Write-Host "  $ObsPluginBin\$PluginDllName"
Write-Host "  $ObsPluginData\en-US.ini"
Write-Host "`n=== OBS Graphics Studio Pro built and installed successfully! ==="
