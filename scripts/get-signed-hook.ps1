# Fetches OBS Studio's official game-capture hook for the OBS version Zunimi builds on, signed by the OBS Project.
# Anti-cheat software (Vanguard for Valorant, and others) refuses to let an unsigned DLL into a game, and the hook we
# build ourselves is unsigned; OBS's signed one is GPL and may be shipped as it is. The hook only talks to the
# win-capture plugin of the same OBS version, so $Version must match the libobs Zunimi is built on.
# Prints the folder that holds the six signed files. Downloads only when they are missing or not validly signed.
param(
    [string]$Version = "33.0.1",
    [string]$Root = "D:\zunimi-build"
)
$ErrorActionPreference = "Stop"
$dir = Join-Path $Root "obs-official"
$out = Join-Path $dir "win-capture-$Version"
$files = "graphics-hook32.dll", "graphics-hook64.dll", "inject-helper32.exe", "inject-helper64.exe",
         "get-graphics-offsets32.exe", "get-graphics-offsets64.exe"

function Test-Signed([string]$folder) {
    foreach ($f in $files) {
        $path = Join-Path $folder $f
        if (-not (Test-Path $path)) { return $false }
        $sig = Get-AuthenticodeSignature $path
        if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch 'CN="?OBS Project') { return $false }
    }
    return $true
}

if (Test-Signed $out) { return $out }

New-Item -ItemType Directory -Force $out | Out-Null
$name = "OBS-Studio-$Version-Windows-x64.zip"
$zip = Join-Path $dir $name
$release = Invoke-RestMethod "https://api.github.com/repos/obsproject/obs-studio/releases/tags/$Version" -Headers @{ "User-Agent" = "ZunimiStudio-build" }
$asset = $release.assets | Where-Object { $_.name -eq $name }
if (-not $asset) { throw "OBS $Version has no $name on GitHub." }
Write-Host "Downloading $name ($([math]::Round($asset.size / 1MB)) MB) for the signed game-capture hook..."
curl.exe -sSfL -o $zip $asset.browser_download_url
if ($LASTEXITCODE) { throw "Downloading $name failed." }
if ($asset.digest) {
    $want = ($asset.digest -replace '^sha256:', '').ToLower()
    $got = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $want) { Remove-Item $zip; throw "$name does not match GitHub's SHA-256 ($got, expected $want)." }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    foreach ($f in $files + "obs-vulkan32.json", "obs-vulkan64.json") {
        $entry = $archive.Entries | Where-Object { $_.FullName -eq "core/win-capture/data/$f" }
        if (-not $entry) { throw "$f is not in $name." }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $out $f), $true)
    }
} finally { $archive.Dispose() }
Remove-Item $zip # 200+ MB we no longer need
if (-not (Test-Signed $out)) { throw "The hook files from $name are not validly signed by the OBS Project." }
Write-Host "Signed game-capture hook for OBS $Version is in $out"
return $out
