$ErrorActionPreference = 'Stop'

$Distro = 'Ubuntu-24.04'
$LinuxScript = '/home/alex/source/mcp-observatory/scripts/daily_registry_refresh.sh'
$LinuxRefreshLogDirectory =
    '/home/alex/source/mcp-observatory/runtime/registry-refresh/logs'

$WindowsLogDirectory = Join-Path $env:USERPROFILE 'McpObservatoryLogs'
$WrapperLog = Join-Path $WindowsLogDirectory 'scheduled-refresh.log'
$FailureMarker = Join-Path $WindowsLogDirectory 'latest-failure.txt'

New-Item -ItemType Directory -Force -Path $WindowsLogDirectory | Out-Null

function Write-WrapperLog {
    param([Parameter(Mandatory)][string]$Message)

    $timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    Add-Content -LiteralPath $WrapperLog -Value "$timestamp $Message"
}

function Show-FailureNotification {
    param([Parameter(Mandatory)][string]$Message)

    try {
        Add-Type -AssemblyName System.Runtime.WindowsRuntime

        $template =
            [Windows.UI.Notifications.ToastTemplateType]::ToastText02

        $xml =
            [Windows.UI.Notifications.ToastNotificationManager]::
                GetTemplateContent($template)

        $textNodes = $xml.GetElementsByTagName('text')

        $textNodes.Item(0).AppendChild(
            $xml.CreateTextNode('MCP Observatory refresh failed')
        ) | Out-Null

        $textNodes.Item(1).AppendChild(
            $xml.CreateTextNode($Message)
        ) | Out-Null

        $toast =
            [Windows.UI.Notifications.ToastNotification]::new($xml)

        [Windows.UI.Notifications.ToastNotificationManager]::
            CreateToastNotifier('Windows PowerShell').Show($toast)
    }
    catch {
        try {
            & "$env:SystemRoot\System32\msg.exe" `
                $env:USERNAME `
                $Message `
                2>$null
        }
        catch {
            Write-WrapperLog `
                "Could not display failure notification: $($_.Exception.Message)"
        }
    }
}

function Get-IdleSeconds {
    Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class UserIdleTime
{
    [StructLayout(LayoutKind.Sequential)]
    private struct LASTINPUTINFO
    {
        public uint cbSize;
        public uint dwTime;
    }

    [DllImport("user32.dll")]
    private static extern bool GetLastInputInfo(ref LASTINPUTINFO info);

    public static uint Seconds()
    {
        LASTINPUTINFO info = new LASTINPUTINFO();
        info.cbSize = (uint)Marshal.SizeOf(info);

        if (!GetLastInputInfo(ref info))
        {
            return 0;
        }

        uint elapsed =
            unchecked((uint)Environment.TickCount - info.dwTime);

        return elapsed / 1000;
    }
}
'@

    return [UserIdleTime]::Seconds()
}

function Suspend-ComputerSafely {
    Add-Type @'
using System.Runtime.InteropServices;

public static class NativePower
{
    [DllImport("powrprof.dll", SetLastError = true)]
    public static extern bool SetSuspendState(
        bool hibernate,
        bool forceCritical,
        bool disableWakeEvent
    );
}
'@

    [void][NativePower]::SetSuspendState($false, $false, $false)
}

function Read-TextFileSafely {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return ''
    }

    return Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
}

function Get-WslUncCandidates {
    param(
        [Parameter(Mandatory)][string]$DistroName,
        [Parameter(Mandatory)][string]$LinuxPath
    )

    $relativePath = $LinuxPath.TrimStart('/') -replace '/', '\'

    @(
        "\\wsl.localhost\$DistroName\$relativePath"
        "\\wsl$\$DistroName\$relativePath"
    )
}

function Get-LatestLinuxErrorLog {
    param(
        [Parameter(Mandatory)][string]$DistroName,
        [Parameter(Mandatory)][string]$LinuxLogDirectory
    )

    $candidateDirectories = Get-WslUncCandidates `
        -DistroName $DistroName `
        -LinuxPath $LinuxLogDirectory

    $uncLogDirectory = $null

    foreach ($candidate in $candidateDirectories) {
        if (Test-Path -LiteralPath $candidate) {
            $uncLogDirectory = $candidate
            break
        }
    }

    if ($null -eq $uncLogDirectory) {
        return (
            "Linux log directory is unavailable through both WSL UNC forms:`r`n" +
            ($candidateDirectories -join "`r`n")
        )
    }

    Write-WrapperLog "Reading Linux error logs through: $uncLogDirectory"

    # Do not use -Filter here. WSL UNC provider behavior can differ by
    # Windows/WSL version, so enumerate first and filter by Name in PowerShell.
    $latestErrorFile =
        Get-ChildItem `
            -LiteralPath $uncLogDirectory `
            -Force `
            -ErrorAction Stop |
        Where-Object {
            -not $_.PSIsContainer -and
            $_.Name.EndsWith(
                '.error',
                [System.StringComparison]::OrdinalIgnoreCase
            )
        } |
        Sort-Object `
            -Property LastWriteTimeUtc `
            -Descending |
        Select-Object -First 1

    if ($null -eq $latestErrorFile) {
        $visibleNames =
            Get-ChildItem `
                -LiteralPath $uncLogDirectory `
                -Force `
                -ErrorAction SilentlyContinue |
            Where-Object { -not $_.PSIsContainer } |
            Select-Object -ExpandProperty Name

        $listing =
            if ($visibleNames) {
                $visibleNames -join ', '
            }
            else {
                '<directory appears empty>'
            }

        return (
            "Check Linux error at mcp-observatory\runtime\registry-refresh\logs\official-refresh-*.error. " +
            "Files visible to PowerShell: $listing"
        )
    }

    $contents =
        Get-Content `
            -LiteralPath $latestErrorFile.FullName `
            -Raw `
            -ErrorAction Stop

    $fullLinuxPath =
        "$($LinuxLogDirectory.TrimEnd('/'))/$($latestErrorFile.Name)"

    if ([string]::IsNullOrWhiteSpace($contents)) {
        return "Latest Linux error file is empty: $fullLinuxPath"
    }

    return (
        "Latest Linux error file: $fullLinuxPath`r`n" +
        $contents.Trim()
    )
}

$exitCode = 1
$runId = Get-Date -Format 'yyyyMMddTHHmmssfff'
$stdoutFile = Join-Path $WindowsLogDirectory "wsl-$runId.stdout.log"
$stderrFile = Join-Path $WindowsLogDirectory "wsl-$runId.stderr.log"

try {
    Write-WrapperLog 'Scheduled refresh started.'

    $wslArguments = @(
        '--distribution'
        $Distro
        '--exec'
        '/bin/bash'
        $LinuxScript
    )

    Write-WrapperLog `
        "Starting WSL command: wsl.exe $($wslArguments -join ' ')"

    $process =
        Start-Process `
            -FilePath "$env:SystemRoot\System32\wsl.exe" `
            -ArgumentList $wslArguments `
            -RedirectStandardOutput $stdoutFile `
            -RedirectStandardError $stderrFile `
            -NoNewWindow `
            -Wait `
            -PassThru

    $exitCode = $process.ExitCode

    $stdout = Read-TextFileSafely -Path $stdoutFile
    $stderr = Read-TextFileSafely -Path $stderrFile

    if (-not [string]::IsNullOrWhiteSpace($stdout)) {
        Write-WrapperLog '----- WSL stdout begin -----'
        Add-Content -LiteralPath $WrapperLog -Value $stdout.TrimEnd()
        Write-WrapperLog '----- WSL stdout end -----'
    }

    if (-not [string]::IsNullOrWhiteSpace($stderr)) {
        Write-WrapperLog '----- WSL stderr begin -----'
        Add-Content -LiteralPath $WrapperLog -Value $stderr.TrimEnd()
        Write-WrapperLog '----- WSL stderr end -----'
    }

    Write-WrapperLog "WSL process exited with code $exitCode."

    if ($exitCode -ne 0) {
        try {
            $linuxErrorLog =
                Get-LatestLinuxErrorLog `
                    -DistroName $Distro `
                    -LinuxLogDirectory $LinuxRefreshLogDirectory
        }
        catch {
            $linuxErrorLog =
                "Could not retrieve the latest Linux error log: " +
                $_.Exception.Message
        }

        $diagnosticParts =
            [System.Collections.Generic.List[string]]::new()

        $diagnosticParts.Add(
            "WSL refresh failed with exit code $exitCode."
        )

        if (-not [string]::IsNullOrWhiteSpace($stderr)) {
            $diagnosticParts.Add(
                "`r`nCaptured WSL stderr:`r`n$($stderr.Trim())"
            )
        }

        if (-not [string]::IsNullOrWhiteSpace($stdout)) {
            $diagnosticParts.Add(
                "`r`nCaptured WSL stdout:`r`n$($stdout.Trim())"
            )
        }

        if (-not [string]::IsNullOrWhiteSpace($linuxErrorLog)) {
            $diagnosticParts.Add(
                "`r`nLatest registry-refresh error log:`r`n" +
                $linuxErrorLog.Trim()
            )
        }

        $fullDiagnostic = $diagnosticParts -join "`r`n"

        Set-Content `
            -LiteralPath $FailureMarker `
            -Value "$(Get-Date -Format o)`r`n$fullDiagnostic"

        Write-WrapperLog `
            '----- Refresh failure diagnostics begin -----'

        Add-Content `
            -LiteralPath $WrapperLog `
            -Value $fullDiagnostic

        Write-WrapperLog `
            '----- Refresh failure diagnostics end -----'

        if (-not [string]::IsNullOrWhiteSpace($stderr)) {
            $notificationDetail = $stderr.Trim()
        }
        elseif (-not [string]::IsNullOrWhiteSpace($linuxErrorLog)) {
            $notificationDetail = $linuxErrorLog.Trim()
        }
        elseif (-not [string]::IsNullOrWhiteSpace($stdout)) {
            $notificationDetail = $stdout.Trim()
        }
        else {
            $notificationDetail =
                "Exit code $exitCode. See $FailureMarker"
        }

        $notificationDetail =
            ($notificationDetail -replace '\s+', ' ').Trim()

        if ($notificationDetail.Length -gt 220) {
            $notificationDetail =
                $notificationDetail.Substring(0, 217) + '...'
        }

        Show-FailureNotification $notificationDetail
    }
    else {
        Remove-Item `
            -LiteralPath $FailureMarker `
            -Force `
            -ErrorAction SilentlyContinue

        Write-WrapperLog `
            'Scheduled refresh completed successfully.'
    }
}
catch {
    $exitCode = 1

    $wrapperFailure =
        "Windows wrapper failed: $($_.Exception.Message)"

    Write-WrapperLog $wrapperFailure

    Set-Content `
        -LiteralPath $FailureMarker `
        -Value "$(Get-Date -Format o)`r`n$wrapperFailure"

    Show-FailureNotification $wrapperFailure
}
finally {
    try {
        $cutoff = (Get-Date).AddDays(-30)

        Get-ChildItem `
            -LiteralPath $WindowsLogDirectory `
            -File `
            -ErrorAction SilentlyContinue |
        Where-Object {
            $_.LastWriteTime -lt $cutoff -and
            $_.Name -ne 'latest-failure.txt'
        } |
        Remove-Item -Force -ErrorAction SilentlyContinue
    }
    catch {
        Write-WrapperLog `
            "Log cleanup failed: $($_.Exception.Message)"
    }

    try {
        Start-Sleep -Seconds 10
        & "$env:SystemRoot\System32\wsl.exe" --shutdown
    }
    catch {
        Write-WrapperLog `
            "WSL shutdown failed: $($_.Exception.Message)"
    }

    try {
        $idleSeconds = Get-IdleSeconds
        $minimumIdleSeconds = 600

        if ($idleSeconds -ge $minimumIdleSeconds) {
            Write-WrapperLog `
                "Computer idle for $idleSeconds seconds; entering sleep."

            Start-Sleep -Seconds 5
            Suspend-ComputerSafely
        }
        else {
            Write-WrapperLog `
                "Computer is active; skipping sleep. Idle seconds=$idleSeconds"
        }
    }
    catch {
        Write-WrapperLog `
            "Idle/sleep handling failed: $($_.Exception.Message)"
    }
}

exit $exitCode
