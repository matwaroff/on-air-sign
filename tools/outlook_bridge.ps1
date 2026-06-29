param(
    [string]$Device,
    [int]$Interval = 0,
    [int]$LookaheadMinutes = 10,
    [switch]$RequireTeamsLink,
    [switch]$IncludeAllDay,
    [switch]$HideSubject
)

$ErrorActionPreference = "Stop"

function Get-EnvInt {
    param(
        [string]$Name,
        [int]$Default
    )

    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($value)) {
        return $Default
    }

    $parsed = 0
    if ([int]::TryParse($value, [ref]$parsed) -and $parsed -gt 0) {
        return $parsed
    }

    return $Default
}

function Get-OutlookApplication {
    try {
        return [Runtime.InteropServices.Marshal]::GetActiveObject("Outlook.Application")
    } catch {
        return New-Object -ComObject Outlook.Application
    }
}

function Get-OptionalComProperty {
    param(
        [object]$Item,
        [string]$Name
    )

    try {
        return $Item.$Name
    } catch {
        return $null
    }
}

function Test-TeamsEvent {
    param([object]$Item)

    $isOnlineMeeting = Get-OptionalComProperty $Item "IsOnlineMeeting"
    if ($isOnlineMeeting -is [bool] -and $isOnlineMeeting) {
        return $true
    }

    foreach ($propertyName in @("OnlineMeetingProvider", "SkypeTeamsMeetingUrl", "JoinOnlineMeetingUrl", "Location", "Body")) {
        $value = Get-OptionalComProperty $Item $propertyName
        if ($null -eq $value) {
            continue
        }

        $text = [string]$value
        if ($text -match "(?i)teams\.microsoft\.com|microsoft teams|skype teams") {
            return $true
        }
    }

    return $false
}

function Format-OutlookDate {
    param([datetime]$Value)

    return $Value.ToString("g", [Globalization.CultureInfo]::CurrentCulture)
}

function Get-ActiveMeeting {
    param(
        [object]$Calendar,
        [int]$WindowMinutes,
        [bool]$OnlyTeams,
        [bool]$AllowAllDay
    )

    $now = Get-Date
    $windowStart = $now.AddMinutes(-2)
    $windowEnd = $now.AddMinutes([Math]::Max($WindowMinutes, 1))

    $items = $Calendar.Items
    $items.Sort("[Start]")
    $items.IncludeRecurrences = $true

    $startText = Format-OutlookDate $windowStart
    $endText = Format-OutlookDate $windowEnd
    $filter = "[End] >= '$startText' AND [Start] <= '$endText'"
    $matches = $items.Restrict($filter)
    $activeMeeting = $null

    foreach ($item in $matches) {
        try {
            $start = [datetime]$item.Start
            $end = [datetime]$item.End

            if (-not ($start -le $now -and $now -lt $end)) {
                continue
            }

            if (-not $AllowAllDay -and [bool]$item.AllDayEvent) {
                continue
            }

            if ([int]$item.BusyStatus -eq 0) {
                continue
            }

            $meetingStatus = Get-OptionalComProperty $item "MeetingStatus"
            if ($null -ne $meetingStatus -and [int]$meetingStatus -eq 5) {
                continue
            }

            if ($OnlyTeams -and -not (Test-TeamsEvent $item)) {
                continue
            }

            if ($null -eq $activeMeeting -or $start -lt [datetime]$activeMeeting.Start) {
                $activeMeeting = $item
            }
        } catch {
            continue
        }
    }

    return $activeMeeting
}

function Send-DeviceState {
    param(
        [string]$BaseUrl,
        [bool]$Active,
        [string]$Subject
    )

    $body = @{
        meetingActive = $Active
        subject = $Subject
    } | ConvertTo-Json -Compress

    return Invoke-RestMethod `
        -Uri "$BaseUrl/api/teams" `
        -Method Post `
        -ContentType "application/json" `
        -Body $body `
        -TimeoutSec 8
}

if ([string]::IsNullOrWhiteSpace($Device)) {
    $Device = [Environment]::GetEnvironmentVariable("ON_AIR_SIGN_URL")
}

if ([string]::IsNullOrWhiteSpace($Device)) {
    $Device = "http://onair.local"
}

if ($Interval -le 0) {
    $Interval = Get-EnvInt "ON_AIR_POLL_SECONDS" 30
}

if ($LookaheadMinutes -le 0) {
    $LookaheadMinutes = Get-EnvInt "ON_AIR_LOOKAHEAD_MINUTES" 10
}

$Device = $Device.TrimEnd("/")
$outlook = Get-OutlookApplication
$calendar = $outlook.Session.GetDefaultFolder(9)
$lastState = $null

Write-Host "Outlook bridge running"
Write-Host "  Device: $Device"
Write-Host "  Calendar: $($calendar.FolderPath)"
Write-Host "  Poll seconds: $([Math]::Max($Interval, 5))"
Write-Host "  Require Teams link: $([bool]$RequireTeamsLink)"

while ($true) {
    try {
        $event = Get-ActiveMeeting `
            -Calendar $calendar `
            -WindowMinutes $LookaheadMinutes `
            -OnlyTeams ([bool]$RequireTeamsLink) `
            -AllowAllDay ([bool]$IncludeAllDay)

        $active = $null -ne $event
        $subject = ""

        if ($active) {
            if ($HideSubject) {
                $subject = "Meeting"
            } else {
                $subject = [string]$event.Subject
            }
        }

        $state = "$active|$subject"
        if ($state -ne $lastState) {
            $response = Send-DeviceState -BaseUrl $Device -Active $active -Subject $subject
            $timestamp = Get-Date -Format "HH:mm:ss"
            $responseJson = $response | ConvertTo-Json -Compress
            Write-Host "$timestamp active=$active subject=""$subject"" response=$responseJson"
            $lastState = $state
        }
    } catch {
        Write-Warning "bridge error: $($_.Exception.Message)"
    }

    Start-Sleep -Seconds ([Math]::Max($Interval, 5))
}
