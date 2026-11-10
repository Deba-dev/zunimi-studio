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
    [switch]$Package
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root "build"
$stage = Join-Path $build "stage"
$env:PATH = "D:\zunimi-build\tools\Scripts;$env:PATH"

cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64 -DCMAKE_GENERATOR_INSTANCE=D:/BuildTools `
    "-DOBS_SOURCE_DIR=$($ObsSource -replace '\\','/')" | Out-Host
if ($LASTEXITCODE) { throw "configure failed" }
cmake --build $build --config $Config --parallel | Out-Host
if ($LASTEXITCODE) { throw "build failed" }

$bin = Join-Path $stage "bin\64bit"
New-Item -ItemType Directory -Force $bin | Out-Null

# libobs runtime and shaders
Copy-Item "$ObsInstall\bin\64bit\*" $bin -Include *.dll, obs-ffmpeg-mux.exe, obs-*-test.exe -Force
Copy-Item "$ObsInstall\data" $stage -Recurse -Force
# only the modules Zunimi loads (keep in sync with CORE_MODULES in src/core/ObsCore.cpp)
$modules = "image-source","obs-ffmpeg","obs-filters","obs-nvenc","obs-outputs","obs-qsv11","obs-text",
           "obs-transitions","obs-x264","rtmp-services","win-capture","win-dshow","win-wasapi"
$core = Join-Path $stage "core"
if (Test-Path $core) { Remove-Item $core -Recurse -Force }
foreach ($m in $modules) { Copy-Item "$ObsInstall\core\$m" (Join-Path $core $m) -Recurse -Force }
Get-ChildItem $stage -Recurse -Filter *.pdb | Where-Object { $_.Name -notin "obs.pdb","zunimi-studio.pdb" } | Remove-Item

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
    Copy-Item (Join-Path $root "LICENSE") $tmp
    $zip = Join-Path $build "ZunimiStudio-$version-windows-x64.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $tmp -DestinationPath $zip -CompressionLevel Optimal
    Write-Host "Package: $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"
}
