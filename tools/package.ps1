# Builds the mod, runs the self-test and writes the Nexus release zip to release\:
#   AutoLockpick-<version>.zip   extract into the game folder (the one with Hellraiser.exe)
# then checks the zip with Windows Defender and writes its Microsoft false-positive submission sheet
# (release\microsoft-<version>\, through ..\defender_check.py: every release is submitted).
#
# Usage (from anywhere):  pwsh -File tools\package.ps1 [-GameExe <path to Hellraiser-Win64-Shipping.exe>]

param(
  [string]$GameExe = "${env:ProgramFiles(x86)}\Steam\steamapps\common\Clive Barker's Hellraiser - Revival\Hellraiser\Binaries\Win64\Hellraiser-Win64-Shipping.exe"
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = Split-Path $PSScriptRoot -Parent
$version = (Select-String -Path "$root\src\version.h" -Pattern 'ALP_VERSION_STRING "([^"]+)"').Matches[0].Groups[1].Value
Write-Host "AutoLockpick for Hellraiser: Revival $version"

# 1. Build.
& cmd /c "`"$root\build.bat`""
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

# 2. Self-test against the installed game: the code search, the vtable hooking, the dsound forwarding.
if (-not (Test-Path -LiteralPath $GameExe)) { throw "Game exe not found: $GameExe (pass -GameExe)" }
& "$root\build\offline_check.exe" $GameExe "$root\build\dsound.dll"
if ($LASTEXITCODE -ne 0) { throw 'self-test failed' }

# The readme in the zip is generated from the Nexus description, so the two say the same thing.
& python -I "$root\tools\bbcode_to_readme.py"
if ($LASTEXITCODE -ne 0) { throw 'readme generation failed' }

# 3. Stage the zip in the game's folder layout.
$stage = "$root\build\package"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$win64 = "$stage\Hellraiser\Binaries\Win64"
New-Item -ItemType Directory -Force $win64 | Out-Null
Copy-Item -LiteralPath "$root\build\dsound.dll" -Destination $win64
$readme = (Get-Content -Raw -Encoding utf8 "$root\release\AutoLockpick_README.txt").Replace('{VERSION}', $version)
$readme = $readme -replace "`r?`n", "`r`n"  # Notepad-friendly line endings
[IO.File]::WriteAllText("$stage\AutoLockpick_README.txt", $readme, [Text.UTF8Encoding]::new($true))

# 4. Zip (System.IO.Compression writes '/' separators, which every extractor and mod manager reads).
New-Item -ItemType Directory -Force "$root\release" | Out-Null
$zip = "$root\release\AutoLockpick-$version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)

# 5. Show what shipped.
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
  Write-Host ("`n{0}  ({1:N0} KB)" -f (Split-Path $zip -Leaf), ((Get-Item -LiteralPath $zip).Length / 1KB))
  $archive.Entries | ForEach-Object { Write-Host ("  {0,-50} {1,10:N0}" -f $_.FullName, $_.Length) }
} finally {
  $archive.Dispose()
}

# 6. Windows Defender, and the Microsoft submission sheet (report only: nothing is quarantined).
& python -I "$root\tools\defender.py" $zip
