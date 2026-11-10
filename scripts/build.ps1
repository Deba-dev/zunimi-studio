# Builds Zunimi Studio and stages a runnable copy in build\stage (the same layout the installer will ship):
#   bin\64bit\zunimi-studio.exe, obs.dll, FFmpeg/x264 DLLs, Qt DLLs
#   core\<module>\<module>.dll + data   (only the modules Zunimi loads)
#   data\libobs\                        (libobs shaders)
# Needs: scripts\build-obs.ps1 run once (libobs built into D:\zunimi-build).
param(
    [string]$ObsSource = "D:\zunimi-build\obs-studio",
    [string]$ObsInstall = "D:\zunimi-build\obs-install",
    [string]$Config = "RelWithDebInfo",
    [switch]$Run,
    [switch]$Package,
    [switch]$Icon,
    [switch]$Browser  # also put the browser source add-on into build\stage (for trying it locally)
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build"
$stage = Join-Path $build "stage"
$env:PATH = "D:\zunimi-build\tools\Scripts;$env:PATH"

cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64 -DCMAKE_GENERATOR_INSTANCE=D:/BuildTools `
    "-DOBS_SOURCE_DIR=$($ObsSource -replace '\\','/')" | Out-Host
if ($LASTEXITCODE) { throw "configure failed" }
if ($Icon) {
    # Re-render the app icon and logo PNGs from resources\logo.svg (they are committed, so this is only needed after a logo change).
    cmake --build $build --config $Config --target make-icon | Out-Host
    if ($LASTEXITCODE) { throw "make-icon failed" }
    $qtBin = (Get-ChildItem "$ObsSource\.deps" -Directory -Filter "obs-deps-qt6-*-x64" | Select-Object -First 1).FullName + "\bin"
    $env:QT_PLUGIN_PATH = (Split-Path $qtBin) + "\plugins"
    $old = $env:PATH; $env:PATH = "$qtBin;$env:PATH"
    & "$build\$Config\make-icon.exe" "$root\resources\logo.svg" "$root\resources" -platform minimal | Out-Host
    $env:PATH = $old
    if ($LASTEXITCODE) { throw "rendering the icon failed" }
}
cmake --build $build --config $Config --parallel | Out-Host
if ($LASTEXITCODE) { throw "build failed" }

$bin = Join-Path $stage "bin\64bit"
New-Item -ItemType Directory -Force $bin | Out-Null

# libobs runtime and shaders
Copy-Item "$ObsInstall\bin\64bit\*" $bin -Include *.dll, obs-ffmpeg-mux.exe, obs-*-test.exe -Force
Copy-Item "$ObsInstall\data" $stage -Recurse -Force
# only the modules Zunimi loads (keep in sync with CORE_MODULES in src/core/ObsCore.cpp)
$modules = "image-source","obs-ffmpeg","obs-filters","obs-nvenc","obs-outputs","obs-qsv11","obs-text",
           "obs-transitions","obs-x264","rtmp-services","win-capture","win-dshow","win-wasapi","obs-webrtc"
$core = Join-Path $stage "core"
if (Test-Path $core) { Remove-Item $core -Recurse -Force }
foreach ($m in $modules) { Copy-Item "$ObsInstall\core\$m" (Join-Path $core $m) -Recurse -Force }
Get-ChildItem $stage -Recurse -Filter *.pdb | Where-Object { $_.Name -notin "obs.pdb","zunimi-studio.pdb" } | Remove-Item

# Game capture: OBS Studio's official hook, signed by the OBS Project, so anti-cheat (Vanguard and others) lets it into
# the game. Our own build of it is unsigned. A release is never made without it.
try {
    $signedHook = & (Join-Path $PSScriptRoot "get-signed-hook.ps1")
    Copy-Item "$signedHook\*" (Join-Path $core "win-capture\data") -Force
} catch {
    if ($Package) { throw "No signed game-capture hook, so no release: $_" }
    Write-Warning "Using the unsigned game-capture hook (anti-cheat games won't allow it): $_"
}

# The browser source (Chromium, ~300 MB) is an add-on: build\addon-browser\core\obs-browser, zipped by -Package as
# ZunimiStudio-BrowserAddon-<version>.zip and downloaded by the app only when someone wants a browser source.
$browserSrc = "$ObsSource\build_zunimi\rundir\$Config\core\obs-browser"
$addon = Join-Path $build "addon-browser"
if (Test-Path "$browserSrc\obs-browser.dll") {
    if (Test-Path $addon) { Remove-Item $addon -Recurse -Force }
    $dst = Join-Path $addon "core\obs-browser"
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item "$browserSrc\*" $dst -Recurse -Force -Exclude *.pdb
    Get-ChildItem $dst -Recurse -Filter *.pdb | Remove-Item
    # Chromium's own UI languages: only the ones Zunimi's users need (the rest is ~60 MB)
    Get-ChildItem "$dst\locales" -File | Where-Object { $_.BaseName -notmatch '^(en-US|en-GB|hi)$' } | Remove-Item
    if ($Browser) { Copy-Item (Join-Path $addon "core\obs-browser") (Join-Path $stage "core\obs-browser") -Recurse -Force }
}

# FFmpeg, x264, curl, srt and friends
$deps = Get-ChildItem "$ObsSource\.deps" -Directory -Filter "obs-deps-20*-x64" | Select-Object -First 1
Copy-Item "$($deps.FullName)\bin\*.dll" $bin -Exclude lua51.dll -Force

# Qt: only what the app uses
$qt = Get-ChildItem "$ObsSource\.deps" -Directory -Filter "obs-deps-qt6-*-x64" | Select-Object -First 1
foreach ($dll in "Qt6Core","Qt6Gui","Qt6Widgets","Qt6Network") { Copy-Item "$($qt.FullName)\bin\$dll.dll" $bin -Force }
foreach ($plugin in "platforms\qwindows.dll","styles\qmodernwindowsstyle.dll","imageformats\qjpeg.dll","imageformats\qico.dll","tls\qschannelbackend.dll") {
    $src = Join-Path "$($qt.FullName)\plugins" $plugin
    if (Test-Path $src) {
        $dst = Join-Path $bin (Split-Path $plugin -Parent)
        New-Item -ItemType Directory -Force $dst | Out-Null
        Copy-Item $src $dst -Force
    }
}

# Microsoft C++ runtime, app-local, so PCs without the VC++ redistributable still start
$crt = Get-ChildItem "D:\BuildTools\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory | Select-Object -Last 1
if ($crt) { Copy-Item "$($crt.FullName)\*.dll" $bin -Force }

Copy-Item "$build\$Config\zunimi-studio.exe", "$build\$Config\zunimi-studio.pdb" $bin -Force
Set-Content (Join-Path $stage "Start Zunimi Studio.cmd") "@start `"`" `"%~dp0bin\64bit\zunimi-studio.exe`"" -Encoding ascii
$mb = [math]::Round((Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
Write-Host "Staged in $stage ($mb MB)"
if ($Run) { Start-Process (Join-Path $bin "zunimi-studio.exe") -WorkingDirectory $bin }
if ($Package) {
    # Release zip: no debug symbols, just what a user needs.
    $version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
    $tmp = Join-Path $build "package\ZunimiStudio-$version"
    if (Test-Path (Split-Path $tmp)) { Remove-Item (Split-Path $tmp) -Recurse -Force }
    Copy-Item $stage $tmp -Recurse
    Get-ChildItem $tmp -Recurse -Filter *.pdb | Remove-Item
    # the browser source ships inside the app (loaded only when someone uses it)
    if (Test-Path "$addon\core\obs-browser") { Copy-Item "$addon\core\obs-browser" "$tmp\core\obs-browser" -Recurse -Force }
    Copy-Item (Join-Path $root "LICENSE") $tmp
    $zip = Join-Path $build "ZunimiStudio-$version-windows-x64.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $tmp -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "Package: $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"

}
