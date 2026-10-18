$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
$vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC C++ Build Tools are required.' }
$vc = (Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$sdk = Join-Path $programFilesX86 'Windows Kits\10'
$sdkVersion = (Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$oldInclude = $env:INCLUDE
$oldLib = $env:LIB
try {
    $env:INCLUDE = "$vc\include;$sdk\Include\$sdkVersion\ucrt;$sdk\Include\$sdkVersion\shared;$sdk\Include\$sdkVersion\um"
    $env:LIB = "$vc\lib\x64;$sdk\Lib\$sdkVersion\ucrt\x64;$sdk\Lib\$sdkVersion\um\x64"
    $build = Join-Path $projectRoot '.esphome\usb-bridge-tests'
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    & "$vc\bin\Hostx64\x64\cl.exe" /nologo /EHsc /std:c++17 /W4 "/I$projectRoot" "/Fo$build\test_hid_keyboard.obj" "/Fe:$build\test_hid_keyboard.exe" "$PSScriptRoot\test_hid_keyboard.cpp"
    if ($LASTEXITCODE -ne 0) { throw 'C++ test compilation failed.' }
    & "$build\test_hid_keyboard.exe"
    if ($LASTEXITCODE -ne 0) { throw 'HID tests failed.' }
} finally {
    $env:INCLUDE = $oldInclude
    $env:LIB = $oldLib
}
