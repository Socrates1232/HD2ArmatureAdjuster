[CmdletBinding()]
param(
    [string]$GameRoot = 'F:\Steam\steamapps\common\Helldivers 2',
    [string]$AddonPath,
    [ValidateRange(10, 900)]
    [int]$ObserveSeconds = 90,
    [ValidateRange(10, 300)]
    [int]$StartupTimeoutSeconds = 120,
    [switch]$AutomateInput,
    [ValidateRange(0, 120)]
    [int]$InputDelaySeconds = 10,
    [switch]$ShutdownAfterTest,
    [switch]$PreflightOnly,
    [switch]$NoDeploy,
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$gameBin = Join-Path $GameRoot 'bin'
$gameExe = Join-Path $gameBin 'helldivers2.exe'
$reshadeDll = Join-Path $gameBin 'dxgi.dll'
$reshadeLog = Join-Path $gameBin 'ReShade.log'
$installedAddon = Join-Path $gameBin 'HD2PaletteProbe.addon64'
$runtimeDirectory = Join-Path $env:LOCALAPPDATA 'HD2ArmatureAdjuster'
$telemetryPath = Join-Path $runtimeDirectory 'telemetry.json'
$automationPath = Join-Path $runtimeDirectory 'automation.request'
$captureNames = @('capture-start.bmp', 'capture-walk.bmp', 'capture-stretch.bmp')
$resultRoot = Join-Path $projectRoot 'test-results'
$sessionName = Get-Date -Format 'yyyyMMdd-HHmmss'
$resultDir = Join-Path $resultRoot $sessionName

function Get-PeMachine([string]$Path) {
    $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $reader = [System.IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { return 0 }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { return 0 }
        return $reader.ReadUInt16()
    }
    finally {
        $stream.Dispose()
    }
}

function Find-AddonBuild {
    $preferred = @(
        (Join-Path $projectRoot 'build\bin\Release\HD2PaletteProbe.addon64'),
        (Join-Path $projectRoot 'build\bin\HD2PaletteProbe.addon64')
    )
    foreach ($path in $preferred) {
        if (Test-Path -LiteralPath $path) { return (Resolve-Path -LiteralPath $path).Path }
    }
    $found = Get-ChildItem -Path (Join-Path $projectRoot 'build*\bin\HD2PaletteProbe.addon64') -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($found) { return $found.FullName }
    throw 'No built HD2PaletteProbe.addon64 was found. Build the Release target first or pass -AddonPath.'
}

function Find-Steam {
    $candidate = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $GameRoot))
    $candidate = Join-Path $candidate 'steam.exe'
    if (Test-Path -LiteralPath $candidate) { return $candidate }
    $registryPath = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamExe
    if ($registryPath -and (Test-Path -LiteralPath $registryPath)) { return $registryPath }
    throw 'steam.exe was not found. Start the game yourself and rerun with -NoLaunch.'
}

function Read-Telemetry {
    if (!(Test-Path -LiteralPath $telemetryPath)) { return $null }
    try {
        return Get-Content -LiteralPath $telemetryPath -Raw | ConvertFrom-Json
    }
    catch {
        return $null
    }
}

function Get-GameProcess {
    foreach ($process in @(Get-Process -Name helldivers2 -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending)) {
        $process.Refresh()
        if (!$process.HasExited) { return $process }
    }
    return $null
}

function Confirm-GameProcess($Expected, [string]$Stage) {
    $current = Get-GameProcess
    if (!$current) { throw "helldivers2.exe stopped before $Stage." }
    if ($current.Id -ne $Expected.Id -or $current.StartTime -ne $Expected.StartTime) {
        throw "helldivers2.exe changed before $Stage; refusing to use stale process state."
    }
    return $current
}

if (!(Test-Path -LiteralPath $gameExe)) { throw "Game executable not found: $gameExe" }
if (!(Test-Path -LiteralPath $reshadeDll)) { throw "ReShade dxgi.dll not found: $reshadeDll" }
if (!$AddonPath) { $AddonPath = Find-AddonBuild }
$AddonPath = (Resolve-Path -LiteralPath $AddonPath).Path

$reshadeVersion = (Get-Item -LiteralPath $reshadeDll).VersionInfo.ProductVersion
if ((Get-PeMachine $AddonPath) -ne 0x8664) { throw 'The add-on is not an x64 PE binary.' }
if ($AutomateInput -and $ObserveSeconds -lt $InputDelaySeconds + 8) {
    throw '-ObserveSeconds must allow at least eight seconds after -InputDelaySeconds.'
}
$addonHash = (Get-FileHash -LiteralPath $AddonPath -Algorithm SHA256).Hash
$running = Get-GameProcess

if (!$NoDeploy) {
    $installedHash = if (Test-Path -LiteralPath $installedAddon) {
        (Get-FileHash -LiteralPath $installedAddon -Algorithm SHA256).Hash
    }
    if ($installedHash -ne $addonHash) {
        if ($running) { throw 'The game is running and the installed add-on differs. Close the game and rerun.' }
        Copy-Item -LiteralPath $AddonPath -Destination $installedAddon -Force
    }
}

if ($PreflightOnly) {
    Write-Host "Preflight passed: ReShade $reshadeVersion, x64 add-on, SHA256 $addonHash"
    exit 0
}

$previousSession = if ($running) { $null } else { (Read-Telemetry).session_id }
$logOffset = if (Test-Path -LiteralPath $reshadeLog) { (Get-Item -LiteralPath $reshadeLog).Length } else { 0 }
$launchedByRunner = $false

if (!$running) {
    if ($NoLaunch) { throw 'Helldivers 2 is not running and -NoLaunch was specified.' }
    $running = Get-GameProcess
}

if (!$running) {
    $steam = Find-Steam
    Write-Host "Launching Helldivers 2 through Steam..."
    Start-Process -FilePath $steam -ArgumentList '-applaunch', '553850'
    $launchedByRunner = $true
    $startupDeadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    do {
        Start-Sleep -Seconds 1
        $running = Get-GameProcess
    } while (!$running -and [DateTime]::UtcNow -lt $startupDeadline)
    if (!$running) { throw "helldivers2.exe did not start within $StartupTimeoutSeconds seconds." }
}
else {
    Write-Host "Attaching to running process $($running.Id)."
}

if ($AutomateInput) {
    $running = Confirm-GameProcess $running 'the input request'
    New-Item -ItemType Directory -Force -Path $runtimeDirectory | Out-Null
    Remove-Item -LiteralPath $automationPath -Force -ErrorAction SilentlyContinue
    foreach ($name in $captureNames) {
        Remove-Item -LiteralPath (Join-Path $runtimeDirectory $name) -Force -ErrorAction SilentlyContinue
    }
    $skipIntro = if ($launchedByRunner) { 1 } else { 0 }
    "$(1000 * $InputDelaySeconds) $skipIntro" | Set-Content -LiteralPath $automationPath -Encoding ascii
    Write-Host "Requested in-game input and low-resolution captures (skip intro: $([bool]$skipIntro))."
}

Write-Host "Observing process $($running.Id) for $ObserveSeconds seconds. Move and turn the character now."
$latestTelemetry = $null
$strongestTelemetry = $null
$maxSelectedChanges = -1
$maxCandidates = 0
$maxMovingCandidates = 0
$lastReportedFrame = -1
$lastReportedAutomation = -1
$lastReportAt = [DateTime]::MinValue
$inputStarted = $false
$inputCompleted = $false
$inputError = $null
$automationStage = 0
$introAttempts = 0
$deadline = [DateTime]::UtcNow.AddSeconds($ObserveSeconds)
while ([DateTime]::UtcNow -lt $deadline) {
    $currentProcess = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
    if (!$currentProcess) { break }
    $currentProcess.Refresh()
    if ($currentProcess.HasExited) { break }
    $sample = Read-Telemetry
    if ($sample -and $sample.process_id -eq $running.Id -and
        ($null -eq $previousSession -or $sample.session_id -ne $previousSession)) {
        $latestTelemetry = $sample
        $maxCandidates = [Math]::Max($maxCandidates, [int]$sample.candidates)
        $maxMovingCandidates = [Math]::Max($maxMovingCandidates, [int]$sample.moving_candidates)
        $automationStage = [Math]::Max($automationStage, [int]$sample.automation_stage)
        $introAttempts = [Math]::Max($introAttempts, [int]$sample.automation_intro_attempts)
        $inputStarted = $inputStarted -or [bool]$sample.automation_input_started
        $inputCompleted = $inputCompleted -or [bool]$sample.automation_completed
        if ([int]$sample.automation_error -ne 0) {
            $inputError = "In-game automation error $($sample.automation_error)."
        }
        if ([int]$sample.selected_changed_slots -gt $maxSelectedChanges -or
            ([int]$sample.selected_changed_slots -eq $maxSelectedChanges -and
             (!$strongestTelemetry -or [int]$sample.candidates -gt [int]$strongestTelemetry.candidates))) {
            $maxSelectedChanges = [int]$sample.selected_changed_slots
            $strongestTelemetry = $sample
        }
        $now = [DateTime]::UtcNow
        if ([int64]$sample.frame -ne $lastReportedFrame -and
            ([int]$sample.automation_stage -ne $lastReportedAutomation -or
             ($now - $lastReportAt).TotalSeconds -ge 5)) {
            $lastReportedFrame = [int64]$sample.frame
            $lastReportedAutomation = [int]$sample.automation_stage
            $lastReportAt = $now
            Write-Host ("frame={0} buffers={1}/{2} candidates={3} moving={4} selected-change={5} automation={6}" -f
                $sample.frame, $sample.mapped_buffers, $sample.tracked_buffers, $sample.candidates,
                $sample.moving_candidates, $sample.selected_changed_slots, $sample.automation_stage)
        }
    }
    Start-Sleep -Seconds 1
}

if ($AutomateInput -and !$inputStarted -and !$inputError) {
    $inputError = 'The in-game automation did not reach the walking stage.'
}
elseif ($AutomateInput -and !$inputCompleted -and !$inputError) {
    $inputError = 'The in-game automation did not complete.'
}
Remove-Item -LiteralPath $automationPath -Force -ErrorAction SilentlyContinue

$logLines = if (Test-Path -LiteralPath $reshadeLog) { @(Get-Content -LiteralPath $reshadeLog -Tail 600) } else { @() }
$interestingLog = @($logLines | Where-Object {
    $_ -match 'HD2PaletteProbe|HD2 Palette Probe|requested API version|Failed to (load|register) add-on'
})
$registerPattern = '\|\s+Registered add-on "HD2 Palette Probe"'
$unregisterPattern = '\|\s+Unregistered add-on "HD2 Palette Probe"'
$registered = [bool]($interestingLog | Where-Object { $_ -match $registerPattern })
$unregistered = [bool]($interestingLog | Where-Object { $_ -match $unregisterPattern })
$lastAddonEvent = $interestingLog | Where-Object { $_ -match $registerPattern -or $_ -match $unregisterPattern } |
    Select-Object -Last 1
$registeredAtEnd = $registered -and $lastAddonEvent -match $registerPattern
$loadError = [bool]($interestingLog | Where-Object { $_ -match 'ERROR.*(HD2PaletteProbe|requested API version|Failed to (load|register) add-on)' })
$telemetrySeen = $null -ne $latestTelemetry
$heartbeatAgeSeconds = if ($telemetrySeen) {
    [Math]::Max(0.0, ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - [double]$latestTelemetry.updated_unix_ms) / 1000.0)
} else { $null }
$heartbeatFresh = $telemetrySeen -and $heartbeatAgeSeconds -le 5.0
$currentProcess = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
if ($currentProcess) { $currentProcess.Refresh() }
$processAlive = $null -ne $currentProcess -and !$currentProcess.HasExited
$runtimeActive = $heartbeatFresh -and $processAlive -and $latestTelemetry.frame -gt 0 -and $latestTelemetry.tracked_buffers -gt 0
$motionDetected = $telemetrySeen -and ($maxMovingCandidates -gt 0 -or $maxSelectedChanges -gt 0)
$status = if ($loadError) { 'failed-load' } elseif (!$registered -and !$telemetrySeen) { 'inconclusive-load' } elseif (!$runtimeActive) { 'failed-runtime' } elseif ($AutomateInput -and ($inputError -or !$inputCompleted)) { 'failed-input' } elseif ($motionDetected) { 'passed-with-motion' } else { 'passed-no-motion-yet' }

New-Item -ItemType Directory -Force -Path $resultDir | Out-Null
$interestingLog | Set-Content -LiteralPath (Join-Path $resultDir 'reshade-tail.log') -Encoding utf8
if ($telemetrySeen) {
    $latestTelemetry | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resultDir 'telemetry.json') -Encoding utf8
}
$captureFiles = @()
foreach ($name in $captureNames) {
    $source = Join-Path $runtimeDirectory $name
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $resultDir $name)
        $captureFiles += $name
    }
}

$shutdownSucceeded = $false
$shutdownError = $null
if ($ShutdownAfterTest) {
    try {
        $target = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
        if ($target) {
            $target.Refresh()
            if (!$target.HasExited) {
                if ($target.StartTime -ne $running.StartTime) { throw 'The process ID was reused; shutdown was refused.' }
                Stop-Process -Id $running.Id -Force -ErrorAction Stop
                Wait-Process -Id $running.Id -Timeout 10 -ErrorAction SilentlyContinue
            }
        }
        $remaining = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
        if ($remaining) { $remaining.Refresh() }
        $shutdownSucceeded = $null -eq $remaining -or $remaining.HasExited
        if (!$shutdownSucceeded) { $shutdownError = 'The tested process did not exit within ten seconds.' }
    }
    catch {
        $shutdownError = $_.Exception.Message
    }
}
if ($ShutdownAfterTest -and !$shutdownSucceeded -and $status -like 'passed-*') {
    $status = 'failed-shutdown'
}

$summary = [ordered]@{
    status = $status
    timestamp = (Get-Date).ToString('o')
    game_process_id = $running.Id
    game_root = $GameRoot
    reshade_version = $reshadeVersion
    required_reshade_api = 17
    addon_sha256 = $addonHash
    addon_registered = $registered
    addon_unregistered = $unregistered
    addon_registered_at_end = $registeredAtEnd
    addon_load_error = $loadError
    log_start_offset = $logOffset
    log_lines_examined = $logLines.Count
    telemetry_seen = $telemetrySeen
    heartbeat_fresh = $heartbeatFresh
    heartbeat_age_seconds = $heartbeatAgeSeconds
    process_alive = $processAlive
    runtime_active = $runtimeActive
    input_requested = [bool]$AutomateInput
    input_started = $inputStarted
    input_completed = $inputCompleted
    input_error = $inputError
    automation_stage = $automationStage
    intro_attempts = $introAttempts
    capture_files = $captureFiles
    shutdown_requested = [bool]$ShutdownAfterTest
    shutdown_succeeded = $shutdownSucceeded
    shutdown_error = $shutdownError
    motion_detected = $motionDetected
    max_candidates = $maxCandidates
    max_moving_candidates = $maxMovingCandidates
    max_selected_changed_slots = [Math]::Max(0, $maxSelectedChanges)
    telemetry = $latestTelemetry
    strongest_telemetry = $strongestTelemetry
}
$summaryPath = Join-Path $resultDir 'summary.json'
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding utf8

Write-Host "Result: $status"
Write-Host "Report: $summaryPath"
if ($status -like 'failed-*' -or $status -like 'inconclusive-*') { exit 1 }
