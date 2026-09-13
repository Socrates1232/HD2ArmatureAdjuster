[CmdletBinding()]
param(
    [int]$TargetProcessId = 0,
    [long]$ExpectedStartTicks = 0,
    [string]$GameRoot = 'F:\Steam\steamapps\common\Helldivers 2',
    [string]$ResultPath,
    [switch]$Elevated
)

$ErrorActionPreference = 'Stop'
$installedAddon = Join-Path (Join-Path $GameRoot 'bin') 'HD2PaletteProbe.addon64'
if (!$ResultPath) {
    $ResultPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'elevated-recovery.json'
}

function Get-TargetProcess([int]$Id) {
    $process = Get-Process -Id $Id -ErrorAction SilentlyContinue
    if ($process) { $process.Refresh() }
    return $process
}

function Test-AddonUnlocked {
    if (!(Test-Path -LiteralPath $installedAddon)) { return $true }
    try {
        $stream = [System.IO.File]::Open($installedAddon, 'Open', 'ReadWrite', 'None')
        $stream.Dispose()
        return $true
    }
    catch {
        return $false
    }
}

if (!$Elevated) {
    $candidates = @(Get-Process -Name helldivers2 -ErrorAction SilentlyContinue)
    if ($candidates.Count -ne 1) {
        throw "Expected exactly one helldivers2.exe process object; found $($candidates.Count)."
    }
    $target = $candidates[0]
    $target.Refresh()
    if (!$target.HasExited) {
        throw "helldivers2.exe PID $($target.Id) is still live. This recovery script only handles already-exited process objects."
    }

    Remove-Item -LiteralPath $ResultPath -Force -ErrorAction SilentlyContinue
    $arguments = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', "`"$PSCommandPath`"",
        '-Elevated',
        '-TargetProcessId', $target.Id,
        '-ExpectedStartTicks', $target.StartTime.Ticks,
        '-GameRoot', "`"$GameRoot`"",
        '-ResultPath', "`"$ResultPath`""
    )
    $elevatedProcess = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" `
        -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    if (!(Test-Path -LiteralPath $ResultPath)) {
        throw "The elevated helper exited with code $($elevatedProcess.ExitCode) without writing a result. The UAC prompt may have been cancelled."
    }
    Get-Content -LiteralPath $ResultPath -Raw
    exit $elevatedProcess.ExitCode
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The recovery helper did not receive administrator rights.'
}

$attempts = [System.Collections.Generic.List[string]]::new()
$target = Get-TargetProcess $TargetProcessId
$identityMatched = $false
$cleared = $false
$restartRequired = $false
$errorText = $null

try {
    if (!$target) {
        $cleared = $true
    }
    else {
        if ($target.StartTime.Ticks -ne $ExpectedStartTicks) {
            throw 'The PID was reused; recovery was refused.'
        }
        if (!$target.HasExited) {
            throw 'The target became live; recovery was refused.'
        }
        $identityMatched = $true

        try {
            Stop-Process -Id $TargetProcessId -Force -ErrorAction Stop
            $attempts.Add('Stop-Process completed')
        }
        catch {
            $attempts.Add("Stop-Process: $($_.Exception.Message)")
        }

        Start-Sleep -Milliseconds 500
        if (Get-TargetProcess $TargetProcessId) {
            try {
                $taskkillOutput = & "$env:SystemRoot\System32\taskkill.exe" /PID $TargetProcessId /F /T 2>&1
                $attempts.Add("taskkill: $($taskkillOutput -join ' ')")
            }
            catch {
                $attempts.Add("taskkill: $($_.Exception.Message)")
            }
        }

        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        do {
            Start-Sleep -Milliseconds 250
            $target = Get-TargetProcess $TargetProcessId
        } while ($target -and [DateTime]::UtcNow -lt $deadline)
        $cleared = $null -eq $target
        $restartRequired = !$cleared
    }
}
catch {
    $errorText = $_.Exception.Message
    $restartRequired = $true
}

$result = [ordered]@{
    timestamp = (Get-Date).ToString('o')
    ran_as_administrator = $true
    target_process_id = $TargetProcessId
    identity_matched = $identityMatched
    process_cleared = $cleared
    addon_unlocked = Test-AddonUnlocked
    restart_required = $restartRequired
    attempts = $attempts
    error = $errorText
}
$result | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $ResultPath -Encoding utf8
if ($cleared -and $result.addon_unlocked) { exit 0 }
exit 2
