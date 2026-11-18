# Gate: frozen API intact, both chips compile, filesystem images build, native tests pass. Checks real exit codes (no piping).
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)
$steps = @(
  @('build nodemcuv2', @('run','-e','nodemcuv2')),
  @('build esp32dev',  @('run','-e','esp32dev')),
  @('fs image nodemcuv2', @('run','-e','nodemcuv2','-t','buildfs')),
  @('fs image esp32dev',  @('run','-e','esp32dev','-t','buildfs')),
  @('native tests',    @('test','-e','native'))
)
Write-Host '== frozen API'
& python tools/check_frozen.py
if ($LASTEXITCODE -ne 0) { Write-Host 'GATE FAIL: frozen API'; exit 1 }
foreach ($s in $steps) {
  Write-Host "== $($s[0])"
  & python -m platformio @($s[1])
  if ($LASTEXITCODE -ne 0) { Write-Host "GATE FAIL: $($s[0])"; exit 1 }
}
Write-Host 'GATE OK'
exit 0
