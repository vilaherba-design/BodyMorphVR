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
$zip = Join-Path $Out "BodyMorphVR-0.8.0.zip"
if (Test-Path $zip) { Remove-Item $zip }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$z = [System.IO.Compression.ZipFile]::Open($zip, "Create")
$C = "00 Core/"
$files = [ordered]@{
  "fomod/info.xml" = "$W\fomod\info.xml"
  "fomod/ModuleConfig.xml" = "$W\fomod\ModuleConfig.xml"
  "${C}F4SE/Plugins/BodyMorphVR.dll" = $dll
  "${C}F4SE/Plugins/BodyMorphVR.pdb" = "$W\build\Release\BodyMorphVR.pdb"
  "${C}F4SE/Plugins/BodyMorphVR.ini" = "$W\data\BodyMorphVR.ini"
  "${C}F4SE/Plugins/BodyMorphVR_LICENSE.txt" = "$W\LICENSE"
  "${C}F4SE/Plugins/BodyMorphVR_README.txt" = "$W\README.md"
  "${C}Scripts/BodyGen.pex" = "$W\data\Scripts\BodyGen.pex"
  "${C}Source/Scripts/BodyGen.psc" = "$W\data\Source\Scripts\BodyGen.psc"
  "${C}F4SE/Plugins/BodyMorphVR_examples/BodyGen/Loose/BodyMorphVR_example_templates.ini" = "$W\examples\BodyGen\Loose\BodyMorphVR_example_templates.ini"
  "${C}F4SE/Plugins/BodyMorphVR_examples/BodyGen/Loose/BodyMorphVR_example_morphs.ini" = "$W\examples\BodyGen\Loose\BodyMorphVR_example_morphs.ini"
}
foreach ($v in @("x1", "x1.5")) {
  foreach ($n in @("Silhouette_templates.ini", "Silhouette_morphs.ini")) {
    $files["10 Silhouette $v/F4SE/Plugins/F4EE/BodyGen/Loose/$n"] = "$W\bodygen\Silhouette-$v\$n"
  }
  $files["10 Silhouette $v/F4SE/Plugins/BodyMorphVR_Silhouette_README.txt"] = "$W\bodygen\Silhouette_README.txt"
  $files["10 Silhouette $v/F4SE/Plugins/BodyMorphVR_Silhouette_LICENSE_GPL-3.0.txt"] = "$W\bodygen\Silhouette_LICENSE_GPL-3.0.txt"
}
# GPL: the port's own source goes inside the mod as a nested zip
$srcZip = Join-Path $W "BodyMorphVR_source.zip"
$sz = [System.IO.Compression.ZipFile]::Open($srcZip, "Create")
foreach ($f in @("CMakeLists.txt", "vcpkg.json", "build.ps1", "README.md", "docs/DEVELOPMENT.md", "LICENSE", "src/main.cpp", "src/pch.h", "src/TriFile.h", "src/TriFile.cpp", "src/MorphApply.h", "src/MorphList.h", "src/MorphStore.h", "src/BodyGenIni.h", "src/BodySlidePresets.h", "src/IniFile.h", "src/TriPath.h", "src/HolderScan.h", "data/BodyMorphVR.ini", "data/Scripts/BodyGen.pex", "data/Source/Scripts/BodyGen.psc", "tools/make_bodygen_pex.py", "tools/scale_templates.py", "examples/BodyGen/Loose/BodyMorphVR_example_templates.ini", "examples/BodyGen/Loose/BodyMorphVR_example_morphs.ini", "fomod/info.xml", "fomod/ModuleConfig.xml", "tests/run.sh", "tests/tri_test.cpp")) {
  [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($sz, (Join-Path $W $f), "BodyMorphVR/$f")
}
$sz.Dispose()
$files["${C}F4SE/Plugins/BodyMorphVR_source.zip"] = $srcZip
foreach ($k in $files.Keys) { [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $files[$k], $k) }
$z.Dispose()
Get-Item $dll, $zip | Format-Table Name, Length
Get-FileHash $zip -Algorithm SHA256 | Format-List
Set-Location $env:TEMP
Remove-Item $W -Recurse -Force  # leave no build tree behind
