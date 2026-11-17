# Exercises the device REST API on a live board. Usage: tools\api_smoke.ps1 -Ip 192.168.4.23 [-Chip esp8266|esp32]
# Adds and removes devices named smoke_*; it leaves nothing behind (and refuses to run if a smoke_* device exists).
param([Parameter(Mandatory)][string]$Ip, [ValidateSet('esp8266', 'esp32')][string]$Chip = 'esp8266')
$ErrorActionPreference = 'Stop'
$script:fail = 0
$script:pass = 0

function Call($method, $path, $body) {
  $args = @{ UseBasicParsing = $true; Method = $method; Uri = "http://$Ip/api/v1$path"; TimeoutSec = 20 }
  if ($body) { $args.Body = ($body | ConvertTo-Json -Compress -Depth 8); $args.ContentType = 'application/json' }
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
function Settle { Start-Sleep -Milliseconds 700 }   # queued ops apply on the next loop() pass
# Waits (up to 10 s) until no smoke_* device is left.
function WaitGone { for ($i = 0; $i -lt 20; $i++) { Start-Sleep -Milliseconds 500; if (-not ((Call GET '/devices').json.devices | Where-Object { $_.id -like 'smoke_*' })) { return } } }
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
Check 'and stopped' ($q.state -eq 'idle' -and $q.stopped -ge 1 -and $q.done -eq 0)
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

Write-Host ("`n{0} passed, {1} failed" -f $script:pass, $script:fail)
exit ([int]($script:fail -gt 0))
