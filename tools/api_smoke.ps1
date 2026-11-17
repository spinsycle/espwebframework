# Exercises the device REST API on a live board. Usage: tools\api_smoke.ps1 -Ip 192.168.4.23 [-Chip esp8266|esp32]
# Adds and removes devices named smoke_*; it leaves nothing behind (and refuses to run if a smoke_* device exists).
param([Parameter(Mandatory)][string]$Ip, [ValidateSet('esp8266', 'esp32')][string]$Chip = 'esp8266')
$ErrorActionPreference = 'Stop'
$script:fail = 0
$script:pass = 0

function Call($method, $path, $body) {
  $args = @{ UseBasicParsing = $true; Method = $method; Uri = "http://$Ip/api/v1$path"; TimeoutSec = 6 }
  if ($body) { $args.Body = ($body | ConvertTo-Json -Compress); $args.ContentType = 'application/json' }
  try {
    $r = Invoke-WebRequest @args
    return @{ code = [int]$r.StatusCode; json = ($r.Content | ConvertFrom-Json) }
  } catch {
    $resp = $_.Exception.Response
    if (-not $resp) { throw }
    $t = $_.ErrorDetails.Message   # PowerShell 5.1 already consumed the response stream; the body is here
    return @{ code = [int]$resp.StatusCode; json = ($(if ($t) { $t | ConvertFrom-Json } else { $null })) }
  }
}
function Check($name, $cond) {
  if ($cond) { $script:pass++; Write-Host "  ok   $name" } else { $script:fail++; Write-Host "  FAIL $name" -ForegroundColor Red }
}
function Settle { Start-Sleep -Milliseconds 400 }   # queued ops apply on the next loop() pass
function Dev($id) { (Call GET '/devices').json.devices | Where-Object { $_.id -eq $id } }

# Pins that are safe to claim on a bare dev board (nothing needs to be wired).
if ($Chip -eq 'esp8266') { $relayPin = 2; $pwmPin = 14; $inPin = 12; $reservedPin = 7; $badPin = 17 }
else                     { $relayPin = 2; $pwmPin = 25; $inPin = 27; $reservedPin = 7; $badPin = 40 }

$existing = (Call GET '/devices').json.devices | Where-Object { $_.id -like 'smoke_*' }
if ($existing) { Write-Host 'smoke_* devices already present; remove them first'; exit 2 }

Write-Host '== validation'
$r = Call POST '/devices' @{ id = 'smoke_led'; type = 'relay'; pin = $relayPin; invert = ($Chip -eq 'esp8266') }
Check 'add relay accepted (202)' ($r.code -eq 202)
Settle
$d = Dev 'smoke_led'
Check 'relay listed, boots off' ($d -and $d.state.on -eq $false)
Check 'name defaults to id' ($d.name -eq 'smoke_led')

$r = Call POST '/devices' @{ id = 'smoke_led'; type = 'relay'; pin = $inPin }
Check 'duplicate id rejected' ($r.code -eq 400 -and $r.json.error -eq 'duplicate_id')
$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'relay'; pin = $relayPin }
Check 'pin in use rejected' ($r.code -eq 400 -and $r.json.error -eq 'pin_in_use')
$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'relay'; pin = $reservedPin }
Check 'flash pin rejected' ($r.code -eq 400 -and $r.json.error -eq 'pin_reserved')
$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'relay'; pin = $badPin }
Check 'nonexistent pin rejected' ($r.code -eq 400)
$r = Call POST '/devices' @{ id = 'Bad Id'; type = 'relay'; pin = $inPin }
Check 'bad id rejected' ($r.code -eq 400 -and $r.json.error -eq 'bad_id')
$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'motor'; pin = $inPin }
Check 'unknown type rejected' ($r.code -eq 400 -and $r.json.error -eq 'bad_type')
$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'relay'; pin = 'five' }
Check 'wrong json type rejected' ($r.code -eq 400)

Write-Host '== relay'
Check 'toggle accepted' ((Call POST '/command' @{ id = 'smoke_led'; action = 'toggle' }).code -eq 202)
Settle
Check 'toggle turned it on' ((Dev 'smoke_led').state.on -eq $true)
Call POST '/command' @{ id = 'smoke_led'; action = 'set'; value = $false } | Out-Null
Settle
Check 'set false turned it off' ((Dev 'smoke_led').state.on -eq $false)
Check 'numeric value rejected for relay' ((Call POST '/command' @{ id = 'smoke_led'; action = 'set'; value = 1 }).code -eq 400)
Check 'unknown action rejected' ((Call POST '/command' @{ id = 'smoke_led'; action = 'explode' }).code -eq 400)
Check 'unknown device 404' ((Call POST '/command' @{ id = 'smoke_nope'; action = 'toggle' }).code -eq 404)

Write-Host '== pwm'
$r = Call POST '/devices' @{ id = 'smoke_pwm'; type = 'pwm'; pin = $pwmPin; freq_hz = 500 }
Check 'add pwm accepted' ($r.code -eq 202)
Settle
Call POST '/command' @{ id = 'smoke_pwm'; action = 'set'; value = 50 } | Out-Null
Settle
Check 'pwm set 50' ((Dev 'smoke_pwm').state.duty -eq 50)
Check 'pwm 101 rejected' ((Call POST '/command' @{ id = 'smoke_pwm'; action = 'set'; value = 101 }).code -eq 400)
Check 'pwm bool rejected' ((Call POST '/command' @{ id = 'smoke_pwm'; action = 'set'; value = $true }).code -eq 400)

Write-Host '== input'
$r = Call POST '/devices' @{ id = 'smoke_in'; type = 'input'; pin = $inPin; pullup = $true; invert = $true; debounce_ms = 30 }
Check 'add input accepted' ($r.code -eq 202)
Settle
$d = Dev 'smoke_in'
Check 'input listed with state' ($d -and $null -ne $d.state.active -and $d.state.edges -eq 0)
Check 'input not commandable (409)' ((Call POST '/command' @{ id = 'smoke_in'; action = 'toggle' }).code -eq 409)

Write-Host '== persistence + removal'
Check 'delete unknown 404' ((Call DELETE '/devices?id=smoke_nope').code -eq 404)
foreach ($id in 'smoke_led', 'smoke_pwm', 'smoke_in') {
  Check "delete $id" ((Call DELETE "/devices?id=$id").code -eq 202)
}
Settle
Check 'all removed' (-not ((Call GET '/devices').json.devices | Where-Object { $_.id -like 'smoke_*' }))
Check 'pin free again after removal' ((Call POST '/devices' @{ id = 'smoke_again'; type = 'relay'; pin = $relayPin }).code -eq 202)
Settle
Call DELETE '/devices?id=smoke_again' | Out-Null
Settle
$h = (Call GET '/status').json.heap
Write-Host ("  heap free {0}, largest block {1}, fragmentation {2}%" -f $h.free, $h.max_block, $h.fragmentation_pct)

Write-Host ("`n{0} passed, {1} failed" -f $script:pass, $script:fail)
exit ([int]($script:fail -gt 0))
