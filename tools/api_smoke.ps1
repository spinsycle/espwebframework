# Exercises the device REST API on a live board. Usage: tools\api_smoke.ps1 -Ip 192.168.4.23 [-Chip esp8266|esp32]
# Adds and removes devices named smoke_*; it leaves nothing behind (and refuses to run if a smoke_* device exists).
param([Parameter(Mandatory)][string]$Ip, [ValidateSet('esp8266', 'esp32')][string]$Chip = 'esp8266',
      [string]$Password = 'espframework', [string]$NewPassword = '', [switch]$Ota)
$ErrorActionPreference = 'Stop'
$script:fail = 0
$script:pass = 0

$script:token = ''
function CallAs($token, $method, $path, $body, $cookies = $null) {
  $args = @{ UseBasicParsing = $true; Method = $method; Uri = "http://$Ip/api/v1$path"; TimeoutSec = 20 }
  if ($token) { $args.Headers = @{ Authorization = "Bearer $token" } }
  if ($cookies) {  # Invoke-WebRequest ignores a hand-written Cookie header: use a cookie jar
    $jar = New-Object Microsoft.PowerShell.Commands.WebRequestSession
    foreach ($k in $cookies.Keys) { $jar.Cookies.Add((New-Object System.Net.Cookie($k, $cookies[$k], '/', $Ip))) }
    $args.WebSession = $jar
  }
  if ($body) { $args.Body = ($body | ConvertTo-Json -Compress -Depth 8); $args.ContentType = 'application/json' }
  # The home Wi-Fi sometimes drops the board for a few seconds. A request with no answer is retried once if it is
  # a read; for anything else (it may have been applied) it counts as code 0, so the check fails but the run goes on.
  for ($try = 1; $try -le 2; $try++) {
    try {
      $r = Invoke-WebRequest @args
      return @{ code = [int]$r.StatusCode; json = ($r.Content | ConvertFrom-Json) }
    } catch {
      $resp = $_.Exception.Response
      if ($resp) {
        $t = $_.ErrorDetails.Message   # PowerShell 5.1 already consumed the response stream; the body is here
        return @{ code = [int]$resp.StatusCode; json = ($(if ($t) { $t | ConvertFrom-Json } else { $null })) }
      }
      if ($method -ne 'GET' -or $try -eq 2) { Write-Host "  (no answer from the board for $method $path)"; return @{ code = 0; json = $null } }
      Start-Sleep -Seconds 4
    }
  }
}
function Call($method, $path, $body) { CallAs $script:token $method $path $body }
function Check($name, $cond) {
  if ($cond) { $script:pass++; Write-Host "  ok   $name" } else { $script:fail++; Write-Host "  FAIL $name" -ForegroundColor Red }
}
function Settle { Start-Sleep -Milliseconds 700 }   # queued ops apply on the next loop() pass
# Waits (up to 10 s) until no smoke_* device is left.
function WaitGone { for ($i = 0; $i -lt 20; $i++) { Start-Sleep -Milliseconds 500; if (-not ((Call GET '/devices').json.devices | Where-Object { $_.id -like 'smoke_*' })) { return } } }
function Dev($id) { (Call GET '/devices').json.devices | Where-Object { $_.id -eq $id } }


# ---- sign in (the board asks for a password on everything under /api/)
$lg = CallAs '' POST '/session' @{ password = $Password }
if ($lg.code -eq 429) { Write-Host "Locked out for $($lg.json.retry_s) s after wrong passwords. Wait and retry."; exit 2 }
if ($lg.code -ne 200) { Write-Host "Login failed (HTTP $($lg.code)). Pass the board's password with -Password."; exit 2 }
$script:token = $lg.json.token
if ($lg.json.must_change) {
  if (-not $NewPassword) { Write-Host 'The board still has the factory password. Pass -NewPassword <8-32 characters> to set one now, or change it in the System tab.'; exit 2 }
  $pc = Call PUT '/password' @{ current = $Password; password = $NewPassword }
  if ($pc.code -ne 200) { Write-Host "Could not set the new password: $($pc.json.error)"; exit 2 }
  $Password = $NewPassword
  Write-Host '  (factory password replaced with -NewPassword)'
}

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
$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'stepper'; pin = $inPin }
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
Write-Host '== P4 devices (nothing needs to be wired: the pins are only configured)'
if ($Chip -eq 'esp8266') { $mEn = 13; $mDir = 16; $sRx = 0; $sTx = 15; $svoPin = 5 } else { $mEn = 26; $mDir = 14; $sRx = 32; $sTx = 33; $svoPin = 4 }

$r = Call POST '/devices' @{ id = 'smoke_mot'; type = 'motor'; pin = $mEn; pin2 = $mDir; freq_hz = 500 }
Check 'add motor accepted' ($r.code -eq 202)
Settle
Check 'motor boots stopped' ((Dev 'smoke_mot').state.speed -eq 0)
Check 'motor set 50' ((Call POST '/command' @{ id = 'smoke_mot'; action = 'set'; value = 50 }).code -eq 202)
Settle
Check 'motor speed 50' ((Dev 'smoke_mot').state.speed -eq 50)
Call POST '/command' @{ id = 'smoke_mot'; action = 'set'; value = -100 } | Out-Null
Settle
Check 'motor reverse full' ((Dev 'smoke_mot').state.speed -eq -100)
Call POST '/command' @{ id = 'smoke_mot'; action = 'stop' } | Out-Null
Settle
Check 'motor stop' ((Dev 'smoke_mot').state.speed -eq 0)
Check 'motor 101 rejected' ((Call POST '/command' @{ id = 'smoke_mot'; action = 'set'; value = 101 }).code -eq 400)
Check 'motor -101 rejected' ((Call POST '/command' @{ id = 'smoke_mot'; action = 'set'; value = -101 }).code -eq 400)
Check 'motor toggle rejected' ((Call POST '/command' @{ id = 'smoke_mot'; action = 'toggle' }).code -eq 400)
$r = Call POST '/devices' @{ id = 'smoke_mot2'; type = 'motor'; pin = $mEn; freq_hz = 500 }
Check 'motor without direction pin rejected' ($r.code -eq 400 -and $r.json.error -eq 'bad_pin')
$r = Call POST '/devices' @{ id = 'smoke_mot2'; type = 'motor'; pin = $svoPin; pin2 = $pwmPin; freq_hz = 500 }  # pin2 belongs to smoke_pwm
Check 'motor direction pin already used rejected' ($r.code -eq 400 -and $r.json.error -eq 'pin_in_use')

$r = Call POST '/devices' @{ id = 'smoke_pca'; type = 'servo'; model = 'pca9685'; pin = -1; i2c_addr = 64; channel = 0 }
Check 'add pca9685 servo accepted' ($r.code -eq 202)
Settle
$d = Dev 'smoke_pca'
Check 'servo boots at 90 degrees' ($d -and $d.state.angle -eq 90 -and $d.model -eq 'pca9685')
Call POST '/command' @{ id = 'smoke_pca'; action = 'set'; value = 20 } | Out-Null
Settle
Check 'servo set 20' ((Dev 'smoke_pca').state.angle -eq 20)
Check 'servo 181 rejected' ((Call POST '/command' @{ id = 'smoke_pca'; action = 'set'; value = 181 }).code -eq 400)
Check 'servo has no stop' ((Call POST '/command' @{ id = 'smoke_pca'; action = 'stop' }).code -eq 400)
$r = Call POST '/devices' @{ id = 'smoke_pca2'; type = 'servo'; model = 'pca9685'; pin = -1; i2c_addr = 64; channel = 16 }
Check 'servo channel 16 rejected' ($r.code -eq 400 -and $r.json.error -eq 'bad_channel')
$r = Call POST '/devices' @{ id = 'smoke_pca2'; type = 'servo'; model = 'pca9685'; pin = -1; i2c_addr = 64; channel = 0 }
Check 'same board and channel rejected' ($r.code -eq 400 -and $r.json.error -eq 'address_in_use')
$r = Call POST '/devices' @{ id = 'smoke_pca2'; type = 'servo'; model = 'pca9685'; pin = -1; i2c_addr = 64; channel = 1 }
Check 'second channel on the same board accepted' ($r.code -eq 202)
Settle
Call DELETE '/devices?id=smoke_pca2' | Out-Null
$r = Call POST '/devices' @{ id = 'smoke_btn4'; type = 'input'; pin = $(if ($Chip -eq 'esp8266') { 4 } else { 21 }) }
Check 'a pin on the I2C bus is refused while an I2C device exists' ($r.code -eq 400 -and $r.json.error -eq 'pin_in_use')

$r = Call POST '/devices' @{ id = 'smoke_bme'; type = 'sensor'; model = 'bme280'; pin = -1; i2c_addr = 118 }
Check 'add bme280 accepted' ($r.code -eq 202)
Settle
$d = Dev 'smoke_bme'
Check 'bme280 listed with a state' ($d -and $null -ne $d.state.ok)
Check 'sensor not commandable (409)' ((Call POST '/command' @{ id = 'smoke_bme'; action = 'set'; value = 1 }).code -eq 409)
$r = Call POST '/devices' @{ id = 'smoke_bme2'; type = 'sensor'; model = 'bme280'; pin = -1; i2c_addr = 118 }
Check 'second sensor on one address rejected' ($r.code -eq 400 -and $r.json.error -eq 'address_in_use')
$r = Call POST '/devices' @{ id = 'smoke_bme2'; type = 'sensor'; model = 'bme280'; pin = -1; i2c_addr = 117 }
Check 'bme280 address 0x75 rejected' ($r.code -eq 400 -and $r.json.error -eq 'bad_i2c_addr')
$r = Call POST '/devices' @{ id = 'smoke_bme2'; type = 'sensor'; model = 'bme280'; pin = -1; i2c_addr = 119; interval_s = 0 }
Check 'interval 0 rejected' ($r.code -eq 400 -and $r.json.error -eq 'bad_interval')
$r = Call POST '/devices' @{ id = 'smoke_bme2'; type = 'sensor'; model = 'lm35'; pin = -1 }
Check 'unknown sensor model rejected' ($r.code -eq 400 -and $r.json.error -eq 'bad_model')

$s = (Call GET '/i2c')
Check 'i2c status has the bus pins' ($s.code -eq 200 -and $s.json.sda -gt 0 -and $s.json.scl -gt 0)

$r = Call POST '/devices' @{ id = 'smoke_ser'; type = 'serial'; pin = $sRx; pin2 = $sTx; baud = 12345 }
Check 'odd baud rejected' ($r.code -eq 400 -and $r.json.error -eq 'bad_baud')
$r = Call POST '/devices' @{ id = 'smoke_ser'; type = 'serial'; pin = $sRx; pin2 = $sTx; baud = 9600 }
Check 'add serial accepted' ($r.code -eq 202)
Settle
$d = Dev 'smoke_ser'
Check 'serial listed with state' ($d -and $d.state.rx_lines -eq 0 -and $d.state.last -eq '')
Check 'serial send accepted' ((Call POST '/command' @{ id = 'smoke_ser'; action = 'send'; value = 'hello' }).code -eq 202)
Check 'serial empty line rejected' ((Call POST '/command' @{ id = 'smoke_ser'; action = 'send'; value = '' }).code -eq 400)
Check 'serial 48 characters rejected' ((Call POST '/command' @{ id = 'smoke_ser'; action = 'send'; value = ('a' * 48) }).code -eq 400)
Check 'serial 47 characters accepted' ((Call POST '/command' @{ id = 'smoke_ser'; action = 'send'; value = ('a' * 47) }).code -eq 202)
Check 'serial set rejected' ((Call POST '/command' @{ id = 'smoke_ser'; action = 'set'; value = 1 }).code -eq 400)
Check 'serial number rejected' ((Call POST '/command' @{ id = 'smoke_ser'; action = 'send'; value = 5 }).code -eq 400)
$r = Call POST '/devices' @{ id = 'smoke_ser2'; type = 'serial'; pin = $svoPin; pin2 = $mDir; baud = 9600 }
Check 'a second serial device rejected' ($r.code -eq 400 -and $r.json.error -eq 'too_many_serial')

# a direct-pin servo needs 50 Hz: the ESP8266 has one PWM frequency (smoke_pwm runs at 1000 Hz), the ESP32 does not
$r = Call POST '/devices' @{ id = 'smoke_svo'; type = 'servo'; pin = $svoPin }
if ($Chip -eq 'esp8266') {
  Check 'direct servo beside a 1000 Hz pwm rejected on esp8266' ($r.code -eq 400 -and $r.json.error -eq 'freq_conflict')
} else {
  Check 'direct servo beside a 1000 Hz pwm accepted on esp32' ($r.code -eq 202)
  Settle
  Call DELETE '/devices?id=smoke_svo' | Out-Null
}

Check 'delete unknown 404' ((Call DELETE '/devices?id=smoke_nope').code -eq 404)
foreach ($id in ((Call GET '/devices').json.devices | Where-Object { $_.id -like 'smoke_*' } | ForEach-Object { $_.id })) {
  Check "delete $id" ((Call DELETE "/devices?id=$id").code -eq 202)
  Start-Sleep -Milliseconds 350   # the board queues 6 changes; a burst of 8 would be refused as busy
}
WaitGone   # each removal saves to flash, so a burst of them takes a second or two
Check 'all removed' (-not ((Call GET '/devices').json.devices | Where-Object { $_.id -like 'smoke_*' }))
Check 'pin free again after removal' ((Call POST '/devices' @{ id = 'smoke_again'; type = 'relay'; pin = $relayPin }).code -eq 202)
Start-Sleep -Milliseconds 1200
Call DELETE '/devices?id=smoke_again' | Out-Null
WaitGone
$h = (Call GET '/status').json.heap
Write-Host ("  heap free {0}, largest block {1}, fragmentation {2}%" -f $h.free, $h.max_block, $h.fragmentation_pct)


Write-Host '== P5 config (edit, export, rejected imports; a good import and a factory reset restart the board, so the checklist does those)'
Check 'edit unknown 404' ((Call PUT '/devices' @{ id = 'smoke_nope'; name = 'x' }).code -eq 404)
Call POST '/devices' @{ id = 'smoke_ed'; type = 'relay'; pin = $relayPin; name = 'before' } | Out-Null
Start-Sleep -Milliseconds 1200
Check 'edit renames in place' ((Call PUT '/devices' @{ id = 'smoke_ed'; name = 'after' }).code -eq 202)
Settle
Check 'edit applied' ((Dev 'smoke_ed').name -eq 'after')
Check 'edit kept the pin' ((Dev 'smoke_ed').pin -eq $relayPin)
Check 'edit cannot change type' ((Call PUT '/devices' @{ id = 'smoke_ed'; type = 'pwm' }).json.error -eq 'bad_type')
Check 'edit validates like add (reserved pin)' ((Call PUT '/devices' @{ id = 'smoke_ed'; pin = $reservedPin }).json.error -eq 'pin_reserved')
Check 'edit to own pin is not a conflict' ((Call PUT '/devices' @{ id = 'smoke_ed'; pin = $relayPin }).code -eq 202)
Settle

$ex = Call GET '/config/export'
Check 'export ok' ($ex.code -eq 200 -and $ex.json.schema_version -eq 1)
Check 'export lists the device' (($ex.json.devices | Where-Object { $_.id -eq 'smoke_ed' }).name -eq 'after')
Check 'export has no live state' (-not ($ex.json.devices | Where-Object { $_.state }))
Check 'export has no passwords' (-not (($ex.json | ConvertTo-Json -Depth 6) -match 'password'))

$before = (Call GET '/devices').json.devices.Count
$r = Call POST '/config/import' @{ schema_version = 99; devices = @() }
Check 'import from a newer schema refused' ($r.code -eq 400 -and $r.json.error -eq 'unsupported_schema')
$r = Call POST '/config/import' @{ schema_version = 1; devices = 'nope' }
Check 'import: devices must be a list' ($r.code -eq 400 -and $r.json.error -eq 'bad_format')
$r = Call POST '/config/import' @{ schema_version = 1; devices = @(
  @{ id = 'imp_a'; type = 'relay'; pin = $relayPin },
  @{ id = 'imp_a'; type = 'relay'; pin = $pwmPin }) }
Check 'import: duplicate id rejected at index 1' ($r.code -eq 400 -and $r.json.error -eq 'import_rejected' -and $r.json.index -eq 1 -and $r.json.reason -eq 'duplicate_id')
$r = Call POST '/config/import' @{ schema_version = 1; devices = @(@{ id = 'imp_a'; type = 'relay'; pin = $reservedPin }) }
Check 'import: bad pin rejected' ($r.code -eq 400 -and $r.json.reason -eq 'pin_reserved')
Settle
Check 'rejected imports changed nothing' (((Call GET '/devices').json.devices.Count -eq $before) -and (Dev 'smoke_ed'))
Check 'factory reset needs the confirm word' ((Call POST '/config/factory-reset' @{ confirm = 'yes' }).json.error -eq 'confirm_required')
Check 'still there after refused reset' ([bool](Dev 'smoke_ed'))
Call DELETE '/devices?id=smoke_ed' | Out-Null
WaitGone


Write-Host '== P6 dashboard (validation only: layouts are stored, nothing moves)'
$sc = Call GET '/dashboard/schema'
Check 'schema has 14 component types' ($sc.json.types.Count -eq 14)
Check 'schema grid is 4 columns' ($sc.json.grid.columns -eq 4)
Check 'schema slider needs a writable level' (($sc.json.types | Where-Object { $_.name -eq 'slider' }).need -eq 'write_level')
$orig = Call GET '/dashboard'
Check 'dashboard readable' ($orig.code -eq 200 -and $orig.json.mode -in 'auto', 'manual')

Call POST '/devices' @{ id = 'smoke_dr'; type = 'relay'; pin = $relayPin } | Out-Null
Call POST '/devices' @{ id = 'smoke_dp'; type = 'pwm'; pin = $pwmPin } | Out-Null
Settle
Check 'relay lists a writable state property' ((Dev 'smoke_dr').props | Where-Object { $_.name -eq 'state' -and $_.write })
Check 'pwm lists a writable level property' ((Dev 'smoke_dp').props | Where-Object { $_.name -eq 'level' -and $_.write })

function Comp($id, $type, $dev, $prop, $col, $row, $w = 1, $h = 1) {
  @{ id = $id; type = $type; label = $id; tab = 0; binding = @{ device = $dev; property = $prop }; layout = @{ column = $col; row = $row; width = $w; height = $h } }
}
function Doc($mode, $comps, $tabs = @()) { @{ schema_version = 1; mode = $mode; tabs = $tabs; components = $comps } }

$good = Doc 'manual' @((Comp 'c1' 'toggle' 'smoke_dr' 'state' 1 1), (Comp 'c2' 'slider' 'smoke_dp' 'level' 2 1 2 1))
$r = Call PUT '/dashboard' $good
Check 'valid manual dashboard saved' ($r.code -eq 200)
$back = Call GET '/dashboard'
Check 'saved dashboard reads back' ($back.json.mode -eq 'manual' -and $back.json.components.Count -eq 2 -and $back.json.components[1].binding.device -eq 'smoke_dp')

$r = Call PUT '/dashboard' (Doc 'manual' @((Comp 'c1' 'toggle' 'smoke_dp' 'level' 1 1)))
Check 'toggle on a pwm refused (bad_property, component 0)' ($r.code -eq 400 -and $r.json.reason -eq 'bad_property' -and $r.json.component -eq 0)
$r = Call PUT '/dashboard' (Doc 'manual' @((Comp 'c1' 'toggle' 'smoke_nope' 'state' 1 1)))
Check 'unknown device refused' ($r.code -eq 400 -and $r.json.reason -eq 'unknown_device')
$r = Call PUT '/dashboard' (Doc 'manual' @((Comp 'c1' 'toggle' 'smoke_dr' 'state' 1 1), (Comp 'c2' 'toggle' 'smoke_dr' 'state' 1 1)))
Check 'overlap refused at component 1' ($r.code -eq 400 -and $r.json.reason -eq 'overlap' -and $r.json.component -eq 1)
$r = Call PUT '/dashboard' (Doc 'manual' @((Comp 'c1' 'toggle' 'smoke_dr' 'state' 1 1), (Comp 'c1' 'toggle' 'smoke_dr' 'state' 2 1)))
Check 'duplicate component id refused' ($r.code -eq 400 -and $r.json.reason -eq 'duplicate_id' -and $r.json.component -eq 1)
$r = Call PUT '/dashboard' (Doc 'manual' @((Comp 'c1' 'toggle' 'smoke_dr' 'state' 4 1 2 1)))
Check 'component off the right edge refused' ($r.code -eq 400 -and $r.json.reason -eq 'bad_layout')
$r = Call PUT '/dashboard' (Doc 'manual' @((Comp 'c1' 'joystick' 'smoke_dp' 'level' 1 1)))
Check 'joystick smaller than 2x2 refused' ($r.code -eq 400 -and $r.json.reason -eq 'bad_layout')
$c = Comp 'c1' 'toggle' 'smoke_dr' 'state' 1 1; $c.tab = 1
$r = Call PUT '/dashboard' (Doc 'manual' @($c))
Check 'tab that does not exist refused' ($r.code -eq 400 -and $r.json.reason -eq 'bad_tab')
$r = Call PUT '/dashboard' (Doc 'manual' @($c) @('Main', 'Main'))
Check 'duplicate tab names refused' ($r.code -eq 400 -and $r.json.reason -eq 'bad_tabs')
$r = Call PUT '/dashboard' (Doc 'sideways' @())
Check 'unknown mode refused' ($r.code -eq 400 -and $r.json.reason -eq 'bad_mode')
$r = Call PUT '/dashboard' (Doc 'manual' @($c) @('Main', 'Garage'))
Check 'a second tab is accepted' ($r.code -eq 200)
$r = Call PUT '/dashboard' (Doc 'manual' @((Comp 'c1' 'toggle' 'smoke_dr' 'state' 1 1)))
$back = Call GET '/dashboard'
$before = $back.json.components.Count
$null = Call PUT '/dashboard' (Doc 'manual' @((Comp 'c1' 'toggle' 'smoke_nope' 'state' 1 1)))
Check 'a refused save changed nothing' ((Call GET '/dashboard').json.components.Count -eq $before -and (Call GET '/dashboard').json.components[0].binding.device -eq 'smoke_dr')

$r = Call PUT '/dashboard' (Doc 'auto' @((Comp 'c1' 'toggle' 'smoke_nope' 'state' 1 1)))
Check 'auto mode stores a layout without checking it' ($r.code -eq 200)
$r = Call PUT '/dashboard' (Doc 'manual' @((Comp 'c1' 'toggle' 'smoke_nope' 'state' 1 1)))
Check 'switching to manual does check it' ($r.code -eq 400)

$null = Call PUT '/dashboard' (Doc 'auto' @())
Call DELETE '/devices?id=smoke_dr' | Out-Null
Call DELETE '/devices?id=smoke_dp' | Out-Null
WaitGone


Write-Host '== P7 automation (rules are created, run and removed; relays are only configured, nothing needs wiring)'
$sch = Call GET '/automation/schema'
Check 'schema lists 9 trigger types' ($sch.json.triggers.Count -eq 9)
Check 'schema limits: 16 rules, 3 conditions, 4 actions' ($sch.json.limits.rules -eq 16 -and $sch.json.limits.conditions -eq 3 -and $sch.json.limits.actions -eq 4)
$cfg0 = Call GET '/automation'
Check 'automation config readable' ($cfg0.code -eq 200 -and $cfg0.json.schema_version -eq 1)
$st = Call GET '/automation/status'
Check 'status has clock, rules and events' ($st.code -eq 200 -and $null -ne $st.json.clock -and $null -ne $st.json.events)

Call POST '/devices' @{ id = 'smoke_ar'; type = 'relay'; pin = $relayPin } | Out-Null
Call POST '/devices' @{ id = 'smoke_ar2'; type = 'relay'; pin = $inPin } | Out-Null
Call POST '/devices' @{ id = 'smoke_ap'; type = 'pwm'; pin = $pwmPin } | Out-Null
Settle

function Rule($id, $trigger, $actions, $conds = @()) {
  @{ id = $id; name = $id; enabled = $true; trigger = $trigger; match = 'all'; conditions = $conds; actions = $actions; cooldown_s = 0 }
}
function RuleStatus($id) { (Call GET '/automation/status').json.status | Where-Object { $_.id -eq $id } }
function Events() { (Call GET '/automation/status').json.events }
$evt = @(@{ type = 'event'; text = 'smoke_ping' })

$r = Call POST '/automation/rules' (Rule 'smoke_a' @{ type = 'manual' } $evt)
Check 'valid rule accepted' ($r.code -eq 202)
Start-Sleep -Milliseconds 1200
$one = (Call GET '/automation').json.rules | Where-Object { $_.id -eq 'smoke_a' }
Check 'rule is stored and listed' ($one -and $one.trigger.type -eq 'manual' -and $one.actions[0].text -eq 'smoke_ping')
Check 'duplicate rule id refused (409)' ((Call POST '/automation/rules' (Rule 'smoke_a' @{ type = 'manual' } $evt)).code -eq 409)

$bad = @(
  @('change on an unknown device', (Rule 'smoke_b' @{ type = 'change'; device = 'smoke_nope'; property = 'state' } $evt), 'unknown_device'),
  @('change on a property the device lacks', (Rule 'smoke_b' @{ type = 'change'; device = 'smoke_ar'; property = 'level' } $evt), 'bad_property'),
  @('when rule without conditions', (Rule 'smoke_b' @{ type = 'when' } $evt), 'bad_conditions'),
  @('interval of 0', (Rule 'smoke_b' @{ type = 'interval'; every_s = 0 } $evt), 'bad_time'),
  @('schedule with a bad time', (Rule 'smoke_b' @{ type = 'schedule'; time = '25:00' } $evt), 'bad_time'),
  @('schedule with no days', (Rule 'smoke_b' @{ type = 'schedule'; time = '10:00'; days = @() } $evt), 'bad_days'),
  @('no actions', (Rule 'smoke_b' @{ type = 'manual' } @()), 'bad_actions'),
  @('pwm set to 101', (Rule 'smoke_b' @{ type = 'manual' } @(@{ type = 'set'; device = 'smoke_ap'; value = 101 })), 'bad_action'),
  @('on for a pwm', (Rule 'smoke_b' @{ type = 'manual' } @(@{ type = 'on'; device = 'smoke_ap' })), 'bad_action'),
  @('action on an unknown device', (Rule 'smoke_b' @{ type = 'manual' } @(@{ type = 'on'; device = 'smoke_nope' })), 'unknown_device'),
  @('event without a name', (Rule 'smoke_b' @{ type = 'manual' } @(@{ type = 'event'; text = '' })), 'bad_value'),
  @('duration on a non-when rule', (Rule 'smoke_b' @{ type = 'interval'; every_s = 5 } $evt @(@{ device = 'smoke_ar'; property = 'state'; operator = '=='; value = 1; for_s = 5 })), 'bad_duration'),
  @('unknown operator', (Rule 'smoke_b' @{ type = 'manual' } $evt @(@{ device = 'smoke_ar'; property = 'state'; operator = '=>'; value = 1 })), 'bad_operator'),
  @('four conditions', (Rule 'smoke_b' @{ type = 'manual' } $evt @(1..4 | ForEach-Object { @{ device = 'smoke_ar'; property = 'state'; operator = '=='; value = 1 } })), 'bad_conditions'),
  @('five actions', (Rule 'smoke_b' @{ type = 'manual' } @(1..5 | ForEach-Object { @{ type = 'event'; text = "e$_" } })), 'bad_actions'),
  @('bad rule id', (Rule 'Bad Id' @{ type = 'manual' } $evt), 'bad_id'),
  @('cooldown too long', @{ id = 'smoke_b'; name = 'x'; trigger = @{ type = 'manual' }; actions = $evt; cooldown_s = 86401 }, 'bad_cooldown')
)
foreach ($b in $bad) {
  $r = Call POST '/automation/rules' $b[1]
  Check ("refused: " + $b[0]) ((($r.code -eq 400) -or ($r.code -eq 409)) -and ($r.json.error -eq $b[2]))
}
Start-Sleep -Milliseconds 800
Check 'refused rules were not stored' (-not ((Call GET '/automation').json.rules | Where-Object { $_.id -eq 'smoke_b' }))

$r = Call PUT '/automation/rules' (Rule 'smoke_a' @{ type = 'interval'; every_s = 3600 } $evt)
Check 'replace accepted' ($r.code -eq 202)
Start-Sleep -Milliseconds 1200
Check 'replace applied' (((Call GET '/automation').json.rules | Where-Object { $_.id -eq 'smoke_a' }).trigger.every_s -eq 3600)
Check 'replace of an unknown rule is 404' ((Call PUT '/automation/rules' (Rule 'smoke_nope' @{ type = 'manual' } $evt)).code -eq 404)

# run it by hand: a manual rule that turns the relay on
$null = Call PUT '/automation/rules' (Rule 'smoke_a' @{ type = 'manual' } @(@{ type = 'on'; device = 'smoke_ar' }))
Start-Sleep -Milliseconds 1200
$r = Call POST '/automation/rules/run?id=smoke_a'
Check 'run accepted' ($r.code -eq 202)
Start-Sleep -Milliseconds 1500
Check 'the rule turned the relay on' ((Dev 'smoke_ar').state.on -eq $true)
$s = RuleStatus 'smoke_a'
Check 'status counts the firing' ($s.fired -ge 1 -and $s.last -eq 'ok')
Check 'the command was confirmed by the device' ($s.unconfirmed -eq 0)
Check 'the event log shows it' ([bool](Events | Where-Object { $_.rule -eq 'smoke_a' -and $_.kind -eq 'fired' }))
Check 'run of an unknown rule is 404' ((Call POST '/automation/rules/run?id=smoke_nope').code -eq 404)

# a when rule watching the relay: it fires once when the relay goes on
$null = Call POST '/automation/rules' (Rule 'smoke_w' @{ type = 'when' } $evt @(@{ device = 'smoke_ar'; property = 'state'; operator = '=='; value = 1 }))
Start-Sleep -Milliseconds 1500
Check 'a when rule fires when its condition is already true' ((RuleStatus 'smoke_w').fired -eq 1)
Start-Sleep -Milliseconds 1200
Check 'it does not fire again while the condition stays true' ((RuleStatus 'smoke_w').fired -eq 1)
Call POST '/command' @{ id = 'smoke_ar'; action = 'set'; value = $false } | Out-Null
Start-Sleep -Milliseconds 1000
Call POST '/command' @{ id = 'smoke_ar'; action = 'set'; value = $true } | Out-Null
Start-Sleep -Milliseconds 1200
Check 'it fires again after the condition went false and true' ((RuleStatus 'smoke_w').fired -eq 2)

# interlock: smoke_ar is on, so a rule may not turn smoke_ar2 on
$r = Call PUT '/automation/settings' @{ interlocks = @(, @('smoke_ar', 'smoke_ar2')) }
Check 'interlock accepted' ($r.code -eq 202)
Start-Sleep -Milliseconds 1200
$null = Call POST '/automation/rules' (Rule 'smoke_i' @{ type = 'manual' } @(@{ type = 'on'; device = 'smoke_ar2' }))
Start-Sleep -Milliseconds 1200
$null = Call POST '/automation/rules/run?id=smoke_i'
Start-Sleep -Milliseconds 1200
Check 'the interlock blocked the rule' ((RuleStatus 'smoke_i').last -eq 'blocked' -and (Dev 'smoke_ar2').state.on -eq $false)
Check 'an interlock must name two different relays' ((Call PUT '/automation/settings' @{ interlocks = @(, @('smoke_ar', 'smoke_ar')) }).json.error -eq 'bad_interlock')
Check 'an interlock must name relays' ((Call PUT '/automation/settings' @{ interlocks = @(, @('smoke_ar', 'smoke_ap')) }).json.error -eq 'bad_interlock')
Check 'utc offset out of range refused' ((Call PUT '/automation/settings' @{ utc_offset_min = 900 }).json.error -eq 'bad_offset')
$null = Call PUT '/automation/settings' @{ interlocks = @() }
Start-Sleep -Milliseconds 1000

# disable / enable and the engine switch
Check 'disable accepted' ((Call POST '/automation/rules/disable?id=smoke_w').code -eq 202)
Start-Sleep -Milliseconds 1200
Check 'the rule list shows it disabled' (((Call GET '/automation').json.rules | Where-Object { $_.id -eq 'smoke_w' }).enabled -eq $false)
Check 'enable accepted' ((Call POST '/automation/rules/enable?id=smoke_w').code -eq 202)
Start-Sleep -Milliseconds 1200
Check 'enable of an unknown rule is 404' ((Call POST '/automation/rules/enable?id=smoke_nope').code -eq 404)
$null = Call PUT '/automation/settings' @{ enabled = $false }
Start-Sleep -Milliseconds 1200
Check 'the engine can be switched off' ((Call GET '/automation/status').json.enabled -eq $false)
$null = Call PUT '/automation/settings' @{ enabled = $true }
Start-Sleep -Milliseconds 1200
Check 'and back on' ((Call GET '/automation/status').json.enabled -eq $true)

# clean up
foreach ($id in 'smoke_a', 'smoke_w', 'smoke_i') {
  Call DELETE "/automation/rules?id=$id" | Out-Null
  Start-Sleep -Milliseconds 1000
}
Check 'rules deleted' (-not ((Call GET '/automation').json.rules | Where-Object { $_.id -like 'smoke_*' }))
Check 'delete of an unknown rule is 404' ((Call DELETE '/automation/rules?id=smoke_nope').code -eq 404)
foreach ($id in 'smoke_ar', 'smoke_ar2', 'smoke_ap') { Call DELETE "/devices?id=$id" | Out-Null }
WaitGone


Write-Host '== P7b sequences and timers (real time: a few seconds each; relays are only configured)'
$sch = Call GET '/automation/schema'
Check 'schema lists the three step types' ($sch.json.step_types.Count -eq 3)
Check 'schema limits: 4 sequences, 16 steps, 32 in all, 6 timers' ($sch.json.limits.sequences -eq 4 -and $sch.json.limits.steps -eq 16 -and $sch.json.limits.steps_total -eq 32 -and $sch.json.limits.timers -eq 6)
Check 'timer is a dashboard component' (($sc.json.types | Where-Object { $_.name -eq 'timer' }).need -eq 'timer')

Call POST '/devices' @{ id = 'smoke_sr'; type = 'relay'; pin = $relayPin } | Out-Null
Call POST '/devices' @{ id = 'smoke_sp'; type = 'pwm'; pin = $pwmPin } | Out-Null
Settle

function Timer($id, $secs, $repeat = $false) { @{ id = $id; name = $id; duration_s = $secs; repeat = $repeat } }
function TimerStatus($id) { (Call GET '/automation/timers').json.timers | Where-Object { $_.id -eq $id } }
function Seq($id, $steps, $repeat = 1) { @{ id = $id; name = $id; repeat = $repeat; steps = $steps } }
function SeqStatus($id) { (Call GET '/automation/status').json.sequences | Where-Object { $_.id -eq $id } }
function SDo($type, $dev, $extra = @{}) { $a = @{ type = $type; device = $dev }; foreach ($k in $extra.Keys) { $a[$k] = $extra[$k] }; @{ type = 'do'; action = $a } }
function SEvent($text) { @{ type = 'do'; action = @{ type = 'event'; text = $text } } }
function SWait($secs) { @{ type = 'wait'; seconds = $secs } }

# ---- timers
$r = Call POST '/automation/timers' (Timer 'smoke_t' 2)
Check 'timer accepted' ($r.code -eq 202)
Start-Sleep -Milliseconds 1200
Check 'timer listed, stopped' ((TimerStatus 'smoke_t') -and (TimerStatus 'smoke_t').running -eq $false -and (TimerStatus 'smoke_t').duration_s -eq 2)
Check 'duplicate timer refused (409)' ((Call POST '/automation/timers' (Timer 'smoke_t' 5)).code -eq 409)
Check 'timer length 0 refused' ((Call POST '/automation/timers' (Timer 'smoke_x' 0)).json.error -eq 'bad_timer')
Check 'timer length 86401 refused' ((Call POST '/automation/timers' (Timer 'smoke_x' 86401)).json.error -eq 'bad_timer')
Check 'bad timer id refused' ((Call POST '/automation/timers' (Timer 'Bad Id' 5)).json.error -eq 'bad_id')
Check 'start of an unknown timer is 404' ((Call POST '/automation/timers/start?id=smoke_nope').code -eq 404)
$null = Call POST '/automation/timers/start?id=smoke_t'
Start-Sleep -Milliseconds 900
$t = TimerStatus 'smoke_t'
Check 'a started timer counts down' ($t.running -eq $true -and $t.remaining_s -le 2 -and $t.remaining_s -ge 1)
Start-Sleep -Milliseconds 2500
$t = TimerStatus 'smoke_t'
Check 'it finishes by itself' ($t.running -eq $false -and $t.finished -ge 1)
Check 'the event log shows timer_done' ([bool](Events | Where-Object { $_.rule -eq 'smoke_t' -and $_.kind -eq 'timer_done' }))
$null = Call POST '/automation/timers/start?id=smoke_t'
Start-Sleep -Milliseconds 700
$null = Call POST '/automation/timers/stop?id=smoke_t'
Start-Sleep -Milliseconds 800
Check 'a stopped timer does not finish' ((TimerStatus 'smoke_t').running -eq $false)
$null = Call PUT '/automation/timers' (Timer 'smoke_t' 30)
Start-Sleep -Milliseconds 1200
Check 'timer replaced' ((TimerStatus 'smoke_t').duration_s -eq 30)
Check 'replace of an unknown timer is 404' ((Call PUT '/automation/timers' (Timer 'smoke_nope' 5)).code -eq 404)

# ---- sequences: validation
$ev = @(SEvent 'x')
$badSeq = @(
  @('no steps', (Seq 'smoke_q' @()), 'bad_steps'),
  @('17 steps', (Seq 'smoke_q' @(1..17 | ForEach-Object { SWait 1 })), 'bad_steps'),
  @('wait of 0', (Seq 'smoke_q' @(SWait 0)), 'bad_time'),
  @('until without a timeout', (Seq 'smoke_q' @(@{ type = 'until'; condition = @{ device = 'smoke_sr'; property = 'state'; operator = '=='; value = 1 }; timeout_s = 0 })), 'bad_time'),
  @('a step on an unknown device', (Seq 'smoke_q' @(SDo 'on' 'smoke_nope')), 'unknown_device'),
  @('pwm set to 101 in a step', (Seq 'smoke_q' @(SDo 'set' 'smoke_sp' @{ value = 101 })), 'bad_action'),
  @('a sequence cannot start a sequence', (Seq 'smoke_q' @(SDo 'seq_start' 'smoke_q')), 'bad_action'),
  @('timer_start on an unknown timer', (Seq 'smoke_q' @(SDo 'timer_start' 'smoke_nope')), 'unknown_target'),
  @('a repeating sequence with no waiting', (Seq 'smoke_q' @(SDo 'on' 'smoke_sr') 3), 'bad_loop'),
  @('repeat forever with no waiting', (Seq 'smoke_q' @(SDo 'on' 'smoke_sr') 0), 'bad_loop'),
  @('bad sequence id', (Seq 'Bad Id' @(SWait 1)), 'bad_id'),
  @('unknown step type', (Seq 'smoke_q' @(@{ type = 'jump' })), 'bad_steps'),
  @('a duration inside a guard', (Seq 'smoke_q' @(@{ type = 'do'; action = @{ type = 'on'; device = 'smoke_sr' }; guard = @{ device = 'smoke_sr'; property = 'state'; operator = '=='; value = 0; for_s = 5 } })), 'bad_duration')
)
foreach ($b in $badSeq) {
  $r = Call POST '/automation/sequences' $b[1]
  Check ("sequence refused: " + $b[0]) ((($r.code -eq 400) -or ($r.code -eq 409)) -and ($r.json.error -eq $b[2]))
}
Start-Sleep -Milliseconds 800
Check 'refused sequences were not stored' (-not ((Call GET '/automation').json.sequences | Where-Object { $_.id -eq 'smoke_q' }))

# ---- a sequence that runs: relay on, wait, event
$null = Call POST '/automation/sequences' (Seq 'smoke_q' @((SDo 'on' 'smoke_sr'), (SWait 1), (SEvent 'smoke_done')))
Start-Sleep -Milliseconds 1200
$one = (Call GET '/automation').json.sequences | Where-Object { $_.id -eq 'smoke_q' }
Check 'sequence stored and listed' ($one -and $one.steps.Count -eq 3 -and $one.steps[1].seconds -eq 1)
Check 'start of an unknown sequence is 404' ((Call POST '/automation/sequences/start?id=smoke_nope').code -eq 404)

# a rule that watches the sequence finish
$null = Call POST '/automation/rules' (Rule 'smoke_sw' @{ type = 'sequence'; device = 'smoke_q'; event = 'done' } @(@{ type = 'event'; text = 'seq_done' }))
Start-Sleep -Milliseconds 1200
Check 'a rule can trigger on a sequence' ([bool](RuleStatus 'smoke_sw'))
Check 'a rule on an unknown sequence is refused' ((Call POST '/automation/rules' (Rule 'smoke_sx' @{ type = 'sequence'; device = 'smoke_nope' } $evt)).json.error -eq 'unknown_target')
Check 'a rule on an unknown timer is refused' ((Call POST '/automation/rules' (Rule 'smoke_sx' @{ type = 'timer'; device = 'smoke_nope' } $evt)).json.error -eq 'unknown_target')
Check 'a rule action on an unknown sequence is refused' ((Call POST '/automation/rules' (Rule 'smoke_sx' @{ type = 'manual' } @(@{ type = 'seq_start'; device = 'smoke_nope' }))).json.error -eq 'unknown_target')

$null = Call POST '/automation/sequences/start?id=smoke_q'
Start-Sleep -Milliseconds 700
$q = SeqStatus 'smoke_q'
Check 'the sequence is running, past step 1' ($q.state -eq 'running' -and $q.step -ge 2)
Check 'its first step turned the relay on' ((Dev 'smoke_sr').state.on -eq $true)
Start-Sleep -Milliseconds 2200
$q = SeqStatus 'smoke_q'
Check 'it finished and counted it' ($q.state -eq 'idle' -and $q.done -ge 1)
Check 'the event log shows sequence_done' ([bool](Events | Where-Object { $_.rule -eq 'smoke_q' -and $_.kind -eq 'sequence_done' }))
Check 'the rule that watches it ran once' ((RuleStatus 'smoke_sw').fired -eq 1)

# ---- pause, resume, stop
$null = Call PUT '/automation/sequences' (Seq 'smoke_q' @((SWait 30), (SEvent 'late')))
Start-Sleep -Milliseconds 1200
$d0 = (SeqStatus 'smoke_q').done   # replacing a sequence keeps its history, so a rule watching it is not disturbed
$null = Call POST '/automation/sequences/start?id=smoke_q'
Start-Sleep -Milliseconds 800
Check 'a long sequence is running' ((SeqStatus 'smoke_q').state -eq 'running')
$null = Call POST '/automation/sequences/pause?id=smoke_q'
Start-Sleep -Milliseconds 800
Check 'it can be paused' ((SeqStatus 'smoke_q').state -eq 'paused')
$null = Call POST '/automation/sequences/resume?id=smoke_q'
Start-Sleep -Milliseconds 800
Check 'and resumed' ((SeqStatus 'smoke_q').state -eq 'running')
$null = Call POST '/automation/sequences/stop?id=smoke_q'
Start-Sleep -Milliseconds 800
$q = SeqStatus 'smoke_q'
Check 'and stopped, without counting as finished' ($q.state -eq 'idle' -and $q.stopped -ge 1 -and $q.done -eq $d0)
Check 'the event log shows sequence_stopped' ([bool](Events | Where-Object { $_.rule -eq 'smoke_q' -and $_.kind -eq 'sequence_stopped' }))

# ---- a rule that starts a timer, a timer rule, and the dashboard component
$null = Call POST '/automation/rules' (Rule 'smoke_ts' @{ type = 'manual' } @(@{ type = 'timer_start'; device = 'smoke_t' }))
$null = Call POST '/automation/rules' (Rule 'smoke_tw' @{ type = 'timer'; device = 'smoke_t' } @(@{ type = 'event'; text = 'tick' }))
Start-Sleep -Milliseconds 1500
$null = Call PUT '/automation/timers' (Timer 'smoke_t' 2)
Start-Sleep -Milliseconds 1200
$null = Call POST '/automation/rules/run?id=smoke_ts'
Start-Sleep -Milliseconds 1000
Check 'a rule can start a timer' ((TimerStatus 'smoke_t').running -eq $true)
Start-Sleep -Milliseconds 2500
Check 'a rule triggers when a timer finishes' ((RuleStatus 'smoke_tw').fired -eq 1)

$tcomp = @{ id = 'c1'; type = 'timer'; label = 'Tea'; tab = 0; binding = @{ device = 'smoke_t' }; layout = @{ column = 1; row = 1; width = 2; height = 1 } }
$r = Call PUT '/dashboard' (Doc 'manual' @($tcomp))
Check 'a dashboard timer component is accepted' ($r.code -eq 200)
$tcomp.binding.device = 'smoke_nope'
$r = Call PUT '/dashboard' (Doc 'manual' @($tcomp))
Check 'a timer component on a missing timer is refused' ($r.code -eq 400 -and $r.json.reason -eq 'unknown_device')
$tcomp.binding.device = 'smoke_t'; $tcomp.layout.width = 1
Check 'a timer component narrower than 2 is refused' ((Call PUT '/dashboard' (Doc 'manual' @($tcomp))).json.reason -eq 'bad_layout')
$null = Call PUT '/dashboard' (Doc 'auto' @())

# ---- the step pool: two sequences of 16 fill it
$null = Call DELETE '/automation/sequences?id=smoke_q'
Start-Sleep -Milliseconds 1000
$big = @(1..16 | ForEach-Object { SWait 1 })
$null = Call POST '/automation/sequences' (Seq 'smoke_b1' $big)
Start-Sleep -Milliseconds 1200
$null = Call POST '/automation/sequences' (Seq 'smoke_b2' $big)
Start-Sleep -Milliseconds 1200
$r = Call POST '/automation/sequences' (Seq 'smoke_b3' @(SWait 1))
Check 'the 32-step pool is full: one more step is refused (409)' ($r.code -eq 409 -and $r.json.error -eq 'too_many')
Check 'the status reports the pool' ((Call GET '/automation/status').json.steps_used -eq 32)

# ---- clean up
foreach ($id in 'smoke_sw', 'smoke_ts', 'smoke_tw') { Call DELETE "/automation/rules?id=$id" | Out-Null; Start-Sleep -Milliseconds 900 }
foreach ($id in 'smoke_b1', 'smoke_b2') { Call DELETE "/automation/sequences?id=$id" | Out-Null; Start-Sleep -Milliseconds 900 }
Call DELETE '/automation/timers?id=smoke_t' | Out-Null
Start-Sleep -Milliseconds 900
Check 'sequences and timers deleted' (-not ((Call GET '/automation').json.sequences | Where-Object { $_.id -like 'smoke_*' }) -and -not (TimerStatus 'smoke_t'))
Check 'delete of an unknown sequence is 404' ((Call DELETE '/automation/sequences?id=smoke_nope').code -eq 404)
Check 'delete of an unknown timer is 404' ((Call DELETE '/automation/timers?id=smoke_nope').code -eq 404)
foreach ($id in 'smoke_sr', 'smoke_sp') { Call DELETE "/devices?id=$id" | Out-Null }
WaitGone


Write-Host '== P8b failsafe, enable/disable, heartbeat, diagnostics (a few real seconds; close other open pages first: their heartbeat would keep the failsafe from firing)'
$r = Call POST '/devices' @{ id = 'smoke_fs'; type = 'relay'; pin = $relayPin; failsafe_s = 3 }
Check 'relay with a failsafe accepted' ($r.code -eq 202)
$r = Call POST '/devices' @{ id = 'smoke_sv'; type = 'servo'; model = 'direct'; pin = $svoPin; failsafe_s = 3; safe_angle = 30 }
Check 'servo with a failsafe angle accepted' ($r.code -eq 202)
Settle
$d = Dev 'smoke_fs'
Check 'config lists enabled and the failsafe' ($d.enabled -eq $true -and $d.failsafe_s -eq 3)
Check 'state shows the failsafe armed, not tripped' ($d.state.failsafe_tripped -eq $false)
Check 'servo lists its safe angle' ((Dev 'smoke_sv').safe_angle -eq 30)

$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'input'; pin = $inPin; failsafe_s = 5 }
Check 'failsafe on an input refused' ($r.code -eq 400 -and $r.json.error -eq 'bad_failsafe')
$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'relay'; pin = $inPin; failsafe_s = 3601 }
Check 'failsafe over 3600 s refused' ($r.code -eq 400 -and $r.json.error -eq 'bad_failsafe')
$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'relay'; pin = $inPin; safe_angle = 90 }
Check 'safe angle on a relay refused' ($r.code -eq 400 -and $r.json.error -eq 'bad_safe_angle')
$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'servo'; model = 'direct'; pin = $inPin; safe_angle = 181 }
Check 'safe angle 181 refused' ($r.code -eq 400 -and $r.json.error -eq 'bad_safe_angle')
$r = Call POST '/devices' @{ id = 'smoke_x'; type = 'relay'; pin = $inPin; enabled = 'yes' }
Check 'enabled must be true or false' ($r.code -eq 400)

Call POST '/command' @{ id = 'smoke_fs'; action = 'set'; value = $true } | Out-Null
Call POST '/command' @{ id = 'smoke_sv'; action = 'set'; value = 120 } | Out-Null
Settle
Check 'relay is on' ((Dev 'smoke_fs').state.on -eq $true)
Check 'servo moved to 120' ((Dev 'smoke_sv').state.angle -eq 120)
for ($i = 0; $i -lt 6; $i++) { Call POST '/heartbeat' @{} | Out-Null; Start-Sleep -Milliseconds 1000 }
Check 'heartbeats keep the failsafe from firing' ((Dev 'smoke_fs').state.on -eq $true -and (Dev 'smoke_fs').state.failsafe_tripped -eq $false)
Check 'servo untouched too' ((Dev 'smoke_sv').state.angle -eq 120)
Start-Sleep -Milliseconds 4500
$d = Dev 'smoke_fs'
Check 'without a heartbeat the relay goes off' ($d.state.on -eq $false)
Check 'and the failsafe shows as tripped' ($d.state.failsafe_tripped -eq $true)
$d = Dev 'smoke_sv'
Check 'the servo went to its failsafe angle' ($d.state.angle -eq 30 -and $d.state.failsafe_tripped -eq $true)
Call POST '/command' @{ id = 'smoke_fs'; action = 'set'; value = $true } | Out-Null
Settle
$d = Dev 'smoke_fs'
Check 'a command resumes it and clears the trip' ($d.state.on -eq $true -and $d.state.failsafe_tripped -eq $false)

$dg = Call GET '/diagnostics'
Check 'diagnostics answers' ($dg.code -eq 200)
Check 'diagnostics has the reset reason' ($dg.json.reset_reason -is [string] -and $dg.json.reset_reason.Length -gt 0)
Check 'diagnostics says whether the watchdog did it' ($null -ne $dg.json.watchdog_reset -and $null -ne $dg.json.fault_reset)
Check 'diagnostics has the longest loop gap' ($dg.json.loop_max_ms -is [int] -or $dg.json.loop_max_ms -is [long])
Check 'diagnostics has the heap low-water mark' ($dg.json.heap.min_free -gt 0 -and $dg.json.heap.min_free -le $dg.json.heap.free + 4096)
Check 'diagnostics lists the devices with their failsafe' (($dg.json.devices | Where-Object { $_.id -eq 'smoke_fs' }).failsafe_s -eq 3)
Check 'the event log recorded the failsafe' ($dg.json.events | Where-Object { $_.kind -eq 'failsafe' -and $_.text -like 'smoke_fs*' })
Check 'the event log counts events since boot' ($dg.json.events_total -ge 1 -and $dg.json.events.Count -ge 1)

Check 'disable accepted' ((Call PUT '/devices' @{ id = 'smoke_fs'; enabled = $false }).code -eq 202)
Settle
$d = Dev 'smoke_fs'
Check 'a disabled device is listed as disabled' ($d.enabled -eq $false)
Check 'and sits in its safe state' ($d.state.on -eq $false)
$r = Call POST '/command' @{ id = 'smoke_fs'; action = 'set'; value = $true }
Check 'a command to it is refused (409 disabled)' ($r.code -eq 409 -and $r.json.error -eq 'disabled')
Check 'a bad command is still a bad command' ((Call POST '/command' @{ id = 'smoke_fs'; action = 'explode' }).code -eq 400)
Check 'the event log recorded the disable' ((Call GET '/diagnostics').json.events | Where-Object { $_.kind -eq 'device' -and $_.text -like 'smoke_fs disabled' })
Check 'enable accepted' ((Call PUT '/devices' @{ id = 'smoke_fs'; enabled = $true }).code -eq 202)
Settle
Check 'enabled again' ((Dev 'smoke_fs').enabled -eq $true)
Check 'it accepts commands again' ((Call POST '/command' @{ id = 'smoke_fs'; action = 'set'; value = $true }).code -eq 202)

$g = Call GET '/gpio'
Check 'gpio inspector answers' ($g.code -eq 200 -and $g.json.read_only -eq $true)
Check 'gpio lists every pin of the chip' ($g.json.pins.Count -ge $(if ($Chip -eq 'esp8266') { 17 } else { 34 }))
$pin = $g.json.pins | Where-Object { $_.gpio -eq $relayPin }
Check 'gpio shows which device uses a pin' ($pin.use -eq 'smoke_fs')
Check 'gpio shows a level for a usable pin' ($null -ne $pin.level)
Check 'gpio does not read a flash pin' ($null -eq ($g.json.pins | Where-Object { $_.gpio -eq 6 }).level -and ($g.json.pins | Where-Object { $_.gpio -eq 6 }).note -eq 'flash')
if ($Chip -eq 'esp8266') {
  Check 'gpio gives the NodeMCU label' (($g.json.pins | Where-Object { $_.gpio -eq 5 }).label -eq 'D1')
  $r = Call POST '/system/test-watchdog' @{ confirm = 'nope' }
  Check 'the watchdog test needs the confirm word' ($r.code -eq 400 -and $r.json.error -eq 'confirm_required')
}

foreach ($ep in @('GET /diagnostics', 'GET /gpio', 'POST /heartbeat', 'POST /system/test-watchdog')) {
  $m, $path = $ep -split ' '
  Check "no login: $ep is 401" ((CallAs '' $m $path $(if ($m -eq 'GET') { $null } else { @{} })).code -eq 401)
}

Call DELETE '/devices?id=smoke_fs' | Out-Null
Start-Sleep -Milliseconds 400
Call DELETE '/devices?id=smoke_sv' | Out-Null
WaitGone


Write-Host '== P8a security (the 5-miss lock-out is tested last and costs a 31 s wait)'
function Anon($method, $path, $body) { CallAs '' $method $path $body }
function Page($path) {
  for ($i = 0; $i -lt 3; $i++) {  # the board refuses new connections for a moment after a burst of requests
    try { return (Invoke-WebRequest -UseBasicParsing -TimeoutSec 20 "http://$Ip$path").StatusCode }
    catch { if ($_.Exception.Response) { return [int]$_.Exception.Response.StatusCode }; Start-Sleep -Seconds 2 }
  }
  return 0
}

Check 'the page itself loads without a login' ((Page '/') -eq 200)
Check 'the page scripts load without a login' ((Page '/bundle.js') -eq 200 -and (Page '/style.css') -eq 200)
$ss = Anon GET '/session'
Check 'GET /session without a token says not authenticated' ($ss.code -eq 200 -and $ss.json.authenticated -eq $false)
$ss = Call GET '/session'
Check 'GET /session with the token says authenticated' ($ss.json.authenticated -eq $true -and $ss.json.must_change -eq $false)

foreach ($p in '/system', '/status', '/devices', '/network', '/config/export', '/dashboard', '/dashboard/schema', '/automation', '/automation/status', '/automation/schema', '/automation/timers', '/i2c') {
  Check "no login: GET $p is 401" ((Anon GET $p).code -eq 401)
}
# Writes with harmless bodies: if one got through it would answer 400 or 404, not 401, and change nothing.
Check 'no login: POST /command is 401' ((Anon POST '/command' @{ id = 'x'; action = 'toggle' }).code -eq 401)
Check 'no login: POST /devices is 401' ((Anon POST '/devices' @{ id = 'x'; type = 'nope' }).code -eq 401)
Check 'no login: PUT /devices is 401' ((Anon PUT '/devices' @{ id = 'x' }).code -eq 401)
Check 'no login: DELETE /devices is 401' ((Anon DELETE '/devices?id=nope').code -eq 401)
Check 'no login: PUT /network is 401' ((Anon PUT '/network' @{ mode = 'nope' }).code -eq 401)
Check 'no login: PUT /dashboard is 401' ((Anon PUT '/dashboard' @{ mode = 'nope' }).code -eq 401)
Check 'no login: POST /config/import is 401' ((Anon POST '/config/import' @{ schema_version = 99; devices = @() }).code -eq 401)
Check 'no login: POST /config/factory-reset is 401' ((Anon POST '/config/factory-reset' @{ confirm = 'no' }).code -eq 401)
Check 'no login: PUT /automation/settings is 401' ((Anon PUT '/automation/settings' @{ utc_offset_min = 9999 }).code -eq 401)
Check 'no login: POST /automation/rules is 401' ((Anon POST '/automation/rules' @{ id = 'Bad Id' }).code -eq 401)
Check 'no login: POST /automation/sequences/start is 401' ((Anon POST '/automation/sequences/start?id=nope').code -eq 401)
Check 'no login: POST /automation/timers/start is 401' ((Anon POST '/automation/timers/start?id=nope').code -eq 401)
Check 'no login: PUT /password is 401' ((Anon PUT '/password' @{ current = 'x'; password = 'y' }).code -eq 401)
Check 'no login: POST /system/restart is 401' ((Anon POST '/system/restart').code -eq 401)

Check 'a made-up token is refused' ((CallAs ('0' * 32) GET '/devices').code -eq 401)
Check 'a token of the wrong length is refused' ((CallAs 'abc' GET '/devices').code -eq 401)
Check 'the session cookie works too' ((CallAs '' GET '/devices' $null @{ sid = $script:token }).code -eq 200)
Check 'among other cookies' ((CallAs '' GET '/devices' $null @{ theme = 'dark'; sid = $script:token; x = '1' }).code -eq 200)
Check 'a cookie whose name only ends in sid is ignored' ((CallAs '' GET '/devices' $null @{ xsid = $script:token }).code -eq 401)

$bad = Anon POST '/session' @{ password = 'definitely-wrong' }
Check 'a wrong password is refused' ($bad.code -eq 401 -and $bad.json.error -eq 'bad_credentials')
Check 'a missing password is refused' ((Anon POST '/session' @{ nothing = 1 }).code -eq 401)

# a second session, then logging it out
$l2 = Anon POST '/session' @{ password = $Password }
$t2 = $l2.json.token
Check 'a second login gives a second session' ($l2.code -eq 200 -and $t2 -and $t2 -ne $script:token)
Check 'both sessions work' ((CallAs $t2 GET '/devices').code -eq 200 -and (Call GET '/devices').code -eq 200)
$out = CallAs $t2 DELETE '/session'
Check 'logout accepted' ($out.code -eq 200)
Check 'a logged-out token stops working' ((CallAs $t2 GET '/devices').code -eq 401)
Check 'the other session is untouched' ((Call GET '/devices').code -eq 200)

# changing the password
$old = $script:token
$l3 = Anon POST '/session' @{ password = $Password }
$other = $l3.json.token
Check 'password change: wrong current is refused' ((Call PUT '/password' @{ current = 'not-it-at-all'; password = 'smoke-temp-pw1' }).json.error -eq 'wrong_password')
Check 'password change: too short is refused' ((Call PUT '/password' @{ current = $Password; password = 'short' }).json.error -eq 'weak_password')
Check 'password change: the factory password is refused' ((Call PUT '/password' @{ current = $Password; password = 'espframework' }).json.error -eq 'default_password')
Check 'password change: 33 characters is refused' ((Call PUT '/password' @{ current = $Password; password = ('x' * 33) }).json.error -eq 'weak_password')
$tmp = 'smoke-temp-pw1'
$ch = Call PUT '/password' @{ current = $Password; password = $tmp }
Check 'password change accepted' ($ch.code -eq 200)
if ($ch.code -eq 200) {
  Check 'the session that changed it stays signed in' ((Call GET '/devices').code -eq 200)
  Check 'every other session was signed out' ((CallAs $other GET '/devices').code -eq 401)
  Check 'the old password no longer works' ((Anon POST '/session' @{ password = $Password }).code -eq 401)
  $ln = Anon POST '/session' @{ password = $tmp }
  Check 'the new password works' ($ln.code -eq 200)
  $back = CallAs $ln.json.token PUT '/password' @{ current = $tmp; password = $Password }
  if ($back.code -ne 200) { Write-Host "  !! COULD NOT RESTORE THE PASSWORD. The board's password is now: $tmp" -ForegroundColor Red }
  Check 'the original password was restored' ($back.code -eq 200)
  $l4 = Anon POST '/session' @{ password = $Password }
  $script:token = $l4.json.token
  Check 'and works again' ($l4.code -eq 200)
}

# firmware update: refusals (nothing here can change what runs)
Add-Type -AssemblyName System.Net.Http
function PostFile($token, $name, [byte[]]$bytes, $query = '') {
  $hc = New-Object System.Net.Http.HttpClient
  $hc.Timeout = [TimeSpan]::FromSeconds(120)
  if ($token) { $hc.DefaultRequestHeaders.Add('Authorization', "Bearer $token") }
  $mp = New-Object System.Net.Http.MultipartFormDataContent
  $fc = New-Object System.Net.Http.ByteArrayContent(, $bytes)
  # The board's multipart parser wants quoted names, as browsers send them; .NET leaves them bare by default.
  $fc.Headers.ContentDisposition = New-Object System.Net.Http.Headers.ContentDispositionHeaderValue 'form-data'
  $fc.Headers.ContentDisposition.Name = '"firmware"'
  $fc.Headers.ContentDisposition.FileName = '"' + $name + '"'
  $mp.Add($fc)
  try {
    $resp = $hc.PostAsync("http://$Ip/api/v1/system/update$query", $mp).Result
    $txt = $resp.Content.ReadAsStringAsync().Result
    return @{ code = [int]$resp.StatusCode; json = ($(try { $txt | ConvertFrom-Json } catch { $null })) }
  } finally { $hc.Dispose() }
}
$junk = [byte[]](1..2000 | ForEach-Object { $_ % 256 })
Check 'firmware: no login is refused' ((PostFile '' 'x.bin' $junk).code -eq 401)
$r = PostFile $script:token 'notes.txt' $junk
Check 'firmware: a file that is not .bin is refused' ($r.code -eq 400 -and $r.json.error -eq 'update_failed')
$r = PostFile $script:token 'x.bin' $junk
Check 'firmware: a file with no image header is refused' ($r.code -eq 400 -and $r.json.error -eq 'update_failed')
$fake = [byte[]](@(0xE9) + (1..3000 | ForEach-Object { ($_ * 7) % 256 }))
$r = PostFile $script:token 'x.bin' $fake '?md5=00000000000000000000000000000000'
Check 'firmware: an image whose checksum is wrong is refused' ($r.code -eq 400 -and $r.json.error -eq 'update_failed')
Check 'the board still answers after the refused updates' ((Call GET '/status').code -eq 200)

# lock-out last
foreach ($i in 1..5) { $null = Anon POST '/session' @{ password = 'wrong-on-purpose' } }
$lk = Anon POST '/session' @{ password = $Password }
Check 'five wrong passwords lock the login (even the right one waits)' ($lk.code -eq 429 -and $lk.json.retry_s -ge 1)
Check 'a session that already exists is not affected by the lock-out' ((Call GET '/devices').code -eq 200)
Start-Sleep -Seconds 32
$lk = Anon POST '/session' @{ password = $Password }
Check 'after 30 s the login is open again' ($lk.code -eq 200)
$script:token = $lk.json.token


if ($Ota) {
  Write-Host '== firmware update (-Ota): installs the firmware built in .pio, then the board restarts'
  $fw = Join-Path (Split-Path $PSScriptRoot -Parent) ".pio\build\$(if ($Chip -eq 'esp8266') { 'nodemcuv2' } else { 'esp32dev' })\firmware.bin"
  if (-not (Test-Path $fw)) { Write-Host "  no firmware at ${fw} - build it first"; $script:fail++ }
  else {
    $bytes = [System.IO.File]::ReadAllBytes($fw)
    $md5 = [BitConverter]::ToString((New-Object System.Security.Cryptography.MD5CryptoServiceProvider).ComputeHash($bytes)).Replace('-', '').ToLower()
    $up0 = (Call GET '/status').json.uptime_s
    $r = PostFile $script:token 'firmware.bin' $bytes "?md5=$md5"
    Check 'firmware installed' ($r.code -eq 200 -and $r.json.ok -eq $true)
    if ($r.code -eq 200) {
      Start-Sleep -Seconds 25
      $back = $null
      foreach ($i in 1..40) { try { $back = Anon GET '/session'; if ($back.code -eq 200) { break } } catch { }; Start-Sleep -Seconds 3 }
      Check 'the board comes back after the update' ($back -and $back.code -eq 200)
      $l = Anon POST '/session' @{ password = $Password }
      $script:token = $l.json.token
      $up1 = (Call GET '/status').json.uptime_s
      Check 'it restarted (uptime went back)' ($up1 -lt $up0 -or $up1 -lt 120)
      Check 'and it still works' ((Call GET '/devices').code -eq 200)
    }
  }
}

Write-Host ("`n{0} passed, {1} failed" -f $script:pass, $script:fail)
exit ([int]($script:fail -gt 0))
