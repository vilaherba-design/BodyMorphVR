# Builds BodyMorphVR in %TEMP%, zips it to %USERPROFILE%\Modding and deletes the build tree.
# Usage (PowerShell): .\build.ps1 -Src <folder with this project> [-CLib <existing CommonLibF4VR checkout at 9057052>]
# Without -CLib it uses lib\CommonLibF4VR (git submodule) or clones it.
param([Parameter(Mandatory)][string]$Src, [string]$Out = (Join-Path $env:USERPROFILE "Modding"), [string]$CLib = "", [switch]$CleanSrc)
$ErrorActionPreference = "Stop"
$W = Join-Path $env:TEMP "bmvr"
if (Test-Path $W) { Remove-Item $W -Recurse -Force }
Copy-Item $Src $W -Recurse
if ($CleanSrc) { Remove-Item $Src -Recurse -Force }  # the source now lives only in %TEMP%\bmvr
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "E:\General\Lifestyle\Modding\Fallout 4\VR Version\Herramientas\vcpkg" }
if (-not $CLib -and (Test-Path "$W\lib\CommonLibF4VR\CommonLibF4\include\REL\Relocation.h")) {
  # the submodule was checked out with the source (git clone --recursive): already copied with it
} elseif ($CLib) {
  # an existing CommonLibF4VR checkout (copied to a short path: cl.exe cannot open 260+ character header paths)
  New-Item -ItemType Directory "$W\lib" -Force | Out-Null
  robocopy $CLib "$W\lib\CommonLibF4VR" /E /XD .git /NFL /NDL /NJH /NJS | Out-Null
} else {
  git clone -q https://github.com/ArthurHub/CommonLibF4VR.git "$W\lib\CommonLibF4VR"
  git -C "$W\lib\CommonLibF4VR" checkout -q 9057052
}
cmake -S $W -B "$W\build" -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE) { throw "configure failed" }
cmake --build "$W\build" --config Release -- /m /v:m
if ($LASTEXITCODE) { throw "build failed" }
$dll = "$W\build\Release\BodyMorphVR.dll"
$zip = Join-Path $Out "BodyMorphVR-0.6.1-bodygen.zip"
if (Test-Path $zip) { Remove-Item $zip }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$z = [System.IO.Compression.ZipFile]::Open($zip, "Create")
$files = @{
  "F4SE/Plugins/BodyMorphVR.dll" = $dll
  "F4SE/Plugins/BodyMorphVR.pdb" = "$W\build\Release\BodyMorphVR.pdb"
  "F4SE/Plugins/BodyMorphVR.ini" = "$W\data\BodyMorphVR.ini"
  "F4SE/Plugins/BodyMorphVR_LICENSE.txt" = "$W\LICENSE"
  "F4SE/Plugins/BodyMorphVR_README.txt" = "$W\README.md"
  "Scripts/BodyGen.pex" = "$W\data\Scripts\BodyGen.pex"
  "Source/Scripts/BodyGen.psc" = "$W\data\Source\Scripts\BodyGen.psc"
  "F4SE/Plugins/BodyMorphVR_examples/BodyGen/Loose/BodyMorphVR_example_templates.ini" = "$W\examples\BodyGen\Loose\BodyMorphVR_example_templates.ini"
  "F4SE/Plugins/BodyMorphVR_examples/BodyGen/Loose/BodyMorphVR_example_morphs.ini" = "$W\examples\BodyGen\Loose\BodyMorphVR_example_morphs.ini"
}
# GPL: the port's own source goes inside the mod as a nested zip
$srcZip = Join-Path $W "BodyMorphVR_source.zip"
$sz = [System.IO.Compression.ZipFile]::Open($srcZip, "Create")
foreach ($f in @("CMakeLists.txt", "vcpkg.json", "build.ps1", "README.md", "LICENSE", "src/main.cpp", "src/pch.h", "src/TriFile.h", "src/TriFile.cpp", "src/MorphApply.h", "src/MorphList.h", "src/MorphStore.h", "src/BodyGenIni.h", "src/IniFile.h", "src/TriPath.h", "src/HolderScan.h", "data/BodyMorphVR.ini", "data/Scripts/BodyGen.pex", "data/Source/Scripts/BodyGen.psc", "tools/make_bodygen_pex.py", "examples/BodyGen/Loose/BodyMorphVR_example_templates.ini", "examples/BodyGen/Loose/BodyMorphVR_example_morphs.ini", "tests/run.sh", "tests/tri_test.cpp")) {
  [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($sz, (Join-Path $W $f), "BodyMorphVR/$f")
}
$sz.Dispose()
$files["F4SE/Plugins/BodyMorphVR_source.zip"] = $srcZip
foreach ($k in $files.Keys) { [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $files[$k], $k) }
$z.Dispose()
Get-Item $dll, $zip | Format-Table Name, Length
Get-FileHash $zip -Algorithm SHA256 | Format-List
Set-Location $env:TEMP
Remove-Item $W -Recurse -Force  # leave no build tree behind
