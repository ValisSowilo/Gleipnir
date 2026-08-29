# Remove gen for the current user.
#
# Takes the install directory off your user PATH and deletes it.  Removes only
# the entry it put there, by exact match, and leaves every other PATH entry
# alone -- an uninstaller that rewrites the whole variable is a good way to
# lose someone's toolchain.
#
# Your archives are not touched.  Nothing here reads or writes .gen files.
#
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1

$ErrorActionPreference = 'Stop'

$dest = Join-Path $env:LOCALAPPDATA 'Programs\gen'
Write-Host "removing gen from $dest"

$old = [Environment]::GetEnvironmentVariable('Path', 'User')
$entries = $old -split ';' | Where-Object { $_ }
if ($entries -contains $dest) {
    $kept = $entries | Where-Object { $_ -ne $dest }
    [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
    Write-Host "  removed from user PATH ($($entries.Count) entries -> $($kept.Count))"
} else {
    Write-Host "  not on user PATH"
}

if (Test-Path $dest) {
    # The script is running from inside the directory it is deleting, so the
    # delete is queued rather than done here: Windows will not remove a folder
    # while a file in it is open.
    $bat = Join-Path $env:TEMP 'gen-uninstall.bat'
    @"
@echo off
ping -n 3 127.0.0.1 >nul
rmdir /s /q "$dest"
del "%~f0"
"@ | Out-File -FilePath $bat -Encoding ascii
    Start-Process -FilePath $bat -WindowStyle Hidden
    Write-Host "  directory removal queued"
} else {
    Write-Host "  directory already gone"
}

Write-Host "done.  Open a new terminal for the PATH change to take effect."
Write-Host "Any .gen archives you made are untouched."
