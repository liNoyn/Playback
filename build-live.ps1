# Builds the live-stream fork with portable toolchains (see docs/live-stream.md).
# Requires Visual Studio 2022 Build Tools (C++); Git, xmake and LLVM are taken from %USERPROFILE%\.toolchains.
$ErrorActionPreference = 'Stop'
$tools = Join-Path $env:USERPROFILE '.toolchains'
$env:PATH = "$tools\mingit\cmd;$tools\xmake-zip\xmake;$tools\llvm\bin;$env:PATH"
if (-not $env:PLAYBACK_VERSION) { $env:PLAYBACK_VERSION = 'v0.3.3-mc26.50-live.2' }

Set-Location $PSScriptRoot
xmake repo -u
if ($LASTEXITCODE -ne 0) { throw "xmake repo -u failed" }
xmake f -y -p windows -a x64 -m release --target_type=client --bundle_ffmpeg=n
if ($LASTEXITCODE -ne 0) { throw "xmake configure failed" }
xmake -y
if ($LASTEXITCODE -ne 0) { throw "xmake build failed" }
Get-ChildItem bin\playback | Select-Object Name, Length
