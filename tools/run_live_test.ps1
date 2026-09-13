[CmdletBinding()]
param(
    [string]$GameRoot = 'F:\Steam\steamapps\common\Helldivers 2',
    [string]$AddonPath,
    [ValidateRange(10, 900)]
    [int]$ObserveSeconds = 90,
    [ValidateRange(10, 300)]
    [int]$StartupTimeoutSeconds = 120,
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
$telemetryPath = Join-Path $env:LOCALAPPDATA 'HD2ArmatureAdjuster\telemetry.json'
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

if (!(Test-Path -LiteralPath $gameExe)) { throw "Game executable not found: $gameExe" }
if (!(Test-Path -LiteralPath $reshadeDll)) { throw "ReShade dxgi.dll not found: $reshadeDll" }
if (!$AddonPath) { $AddonPath = Find-AddonBuild }
$AddonPath = (Resolve-Path -LiteralPath $AddonPath).Path

$reshadeVersion = (Get-Item -LiteralPath $reshadeDll).VersionInfo.ProductVersion
if ((Get-PeMachine $AddonPath) -ne 0x8664) { throw 'The add-on is not an x64 PE binary.' }
$addonHash = (Get-FileHash -LiteralPath $AddonPath -Algorithm SHA256).Hash
$running = Get-Process -Name helldivers2 -ErrorAction SilentlyContinue | Select-Object -First 1

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

$previousSession = (Read-Telemetry).session_id
$logOffset = if (Test-Path -LiteralPath $reshadeLog) { (Get-Item -LiteralPath $reshadeLog).Length } else { 0 }

if (!$running) {
    if ($NoLaunch) { throw 'Helldivers 2 is not running and -NoLaunch was specified.' }
    $steam = Find-Steam
    Write-Host "Launching Helldivers 2 through Steam..."
    Start-Process -FilePath $steam -ArgumentList '-applaunch', '553850'
    $startupDeadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    do {
        Start-Sleep -Seconds 1
        $running = Get-Process -Name helldivers2 -ErrorAction SilentlyContinue | Select-Object -First 1
    } while (!$running -and [DateTime]::UtcNow -lt $startupDeadline)
    if (!$running) { throw "helldivers2.exe did not start within $StartupTimeoutSeconds seconds." }
}
else {
    Write-Host "Attaching to running process $($running.Id)."
}

Write-Host "Observing process $($running.Id) for $ObserveSeconds seconds. Move and turn the character now."
$latestTelemetry = $null
$strongestTelemetry = $null
$maxSelectedChanges = -1
$maxCandidates = 0
$deadline = [DateTime]::UtcNow.AddSeconds($ObserveSeconds)
while ([DateTime]::UtcNow -lt $deadline) {
    if (!(Get-Process -Id $running.Id -ErrorAction SilentlyContinue)) { break }
    $sample = Read-Telemetry
    if ($sample -and $sample.process_id -eq $running.Id -and $sample.session_id -ne $previousSession) {
        $latestTelemetry = $sample
        $maxCandidates = [Math]::Max($maxCandidates, [int]$sample.candidates)
        if ([int]$sample.selected_changed_slots -gt $maxSelectedChanges) {
            $maxSelectedChanges = [int]$sample.selected_changed_slots
            $strongestTelemetry = $sample
        }
        Write-Host ("frame={0} buffers={1}/{2} candidates={3} moving={4} selected-change={5}" -f
            $sample.frame, $sample.mapped_buffers, $sample.tracked_buffers, $sample.candidates,
            $sample.moving_candidates, $sample.selected_changed_slots)
    }
    Start-Sleep -Seconds 1
}

$logLines = if (Test-Path -LiteralPath $reshadeLog) { @(Get-Content -LiteralPath $reshadeLog -Tail 600) } else { @() }
$interestingLog = @($logLines | Where-Object {
    $_ -match 'HD2PaletteProbe|HD2 Palette Probe|requested API version|Failed to (load|register) add-on'
})
$registered = [bool]($interestingLog | Where-Object { $_ -match 'Registered add-on "HD2 Palette Probe"' })
$loadError = [bool]($interestingLog | Where-Object { $_ -match 'ERROR.*(HD2PaletteProbe|requested API version|Failed to (load|register) add-on)' })
$telemetrySeen = $null -ne $latestTelemetry
$runtimeActive = $telemetrySeen -and $latestTelemetry.frame -gt 0 -and $latestTelemetry.tracked_buffers -gt 0
$motionDetected = $telemetrySeen -and ($latestTelemetry.moving_candidates -gt 0 -or $maxSelectedChanges -gt 0)
$status = if ($loadError) { 'failed-load' } elseif (!$registered -and !$telemetrySeen) { 'inconclusive-load' } elseif (!$runtimeActive) { 'failed-runtime' } elseif ($motionDetected) { 'passed-with-motion' } else { 'passed-no-motion-yet' }

New-Item -ItemType Directory -Force -Path $resultDir | Out-Null
$interestingLog | Set-Content -LiteralPath (Join-Path $resultDir 'reshade-tail.log') -Encoding utf8
if ($telemetrySeen) {
    $latestTelemetry | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resultDir 'telemetry.json') -Encoding utf8
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
    addon_load_error = $loadError
    log_start_offset = $logOffset
    log_lines_examined = $logLines.Count
    telemetry_seen = $telemetrySeen
    runtime_active = $runtimeActive
    motion_detected = $motionDetected
    max_candidates = $maxCandidates
    max_selected_changed_slots = [Math]::Max(0, $maxSelectedChanges)
    telemetry = $latestTelemetry
    strongest_telemetry = $strongestTelemetry
}
$summaryPath = Join-Path $resultDir 'summary.json'
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding utf8

Write-Host "Result: $status"
Write-Host "Report: $summaryPath"
if ($status -like 'failed-*' -or $status -like 'inconclusive-*') { exit 1 }
