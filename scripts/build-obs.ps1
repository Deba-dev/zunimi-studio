# One-time setup: builds libobs and the core OBS plugins that Zunimi Studio uses, all on D:.
# Needs: VS 2022 Build Tools with the C++ workload in D:\BuildTools, Git, Python 3.12 (for CMake).
#   winget install Microsoft.VisualStudio.2022.BuildTools --override "--installPath D:\BuildTools
#     --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.ATL
#     --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --nocache --passive --wait"
param(
    [string]$Root = "D:\zunimi-build",
    [string]$ObsTag = "33.0.1"
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $Root | Out-Null

# CMake 3.30 in its own venv (OBS needs 3.28-3.30)
if (-not (Test-Path "$Root\tools\Scripts\cmake.exe")) {
    py -3.12 -m venv "$Root\tools"
    & "$Root\tools\Scripts\python.exe" -m pip install -q "cmake>=3.30,<3.31"
}
$env:PATH = "$Root\tools\Scripts;$env:PATH"

$obs = Join-Path $Root "obs-studio"
if (-not (Test-Path $obs)) {
    git clone --depth 1 --branch $ObsTag --recurse-submodules --shallow-submodules `
        https://github.com/obsproject/obs-studio.git $obs
}

# OBS 33's presets target VS 2026; this preset builds with VS 2022 and leaves out everything Zunimi
# does not load: OBS's own UI, scripting, VST, websocket, capture cards, NVIDIA FX. The browser source is built
# (CEF, Chromium, is downloaded by OBS's CMake) but ships as an add-on that Zunimi loads only when switched on.
@'
{
  "version": 8,
  "configurePresets": [
    {
      "name": "zunimi",
      "inherits": "windows-x64",
      "generator": "Visual Studio 17 2022",
      "binaryDir": "${sourceDir}/build_zunimi",
      "cacheVariables": {
        "CMAKE_GENERATOR_INSTANCE": "D:/BuildTools",
        "ENABLE_FRONTEND": false, "ENABLE_BROWSER": true, "ENABLE_BROWSER_PANELS": true, "ENABLE_SCRIPTING": false,
        "ENABLE_AJA": false, "ENABLE_DECKLINK": false, "ENABLE_VST": false, "ENABLE_VLC": false,
        "ENABLE_WEBSOCKET": false, "ENABLE_TEST_INPUT": false, "ENABLE_NVAFX": false, "ENABLE_NVVFX": false
      }
    }
  ]
}
'@ | Set-Content (Join-Path $obs "CMakeUserPresets.json") -Encoding ascii

Push-Location $obs
try {
    cmake --preset zunimi
    if ($LASTEXITCODE) { throw "OBS configure failed" }
    cmake --build build_zunimi --config RelWithDebInfo --parallel
    if ($LASTEXITCODE) { throw "OBS build failed" }
    cmake --install build_zunimi --config RelWithDebInfo --prefix "$Root\obs-install"
} finally { Pop-Location }
# OBS's signed game-capture hook for this version (see get-signed-hook.ps1); build.ps1 fetches it too if it is missing.
& (Join-Path $PSScriptRoot "get-signed-hook.ps1") -Version $ObsTag -Root $Root | Out-Null
Write-Host "libobs is ready. Now run scripts\build.ps1"
