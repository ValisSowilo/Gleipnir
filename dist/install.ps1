# Install gleipnir for the current user.
#
# Copies the program OUT of wherever you unpacked it and into
#   %LOCALAPPDATA%\Programs\gleipnir
# then puts that directory on your user PATH.
#
# The copy is the point.  Pointing PATH at a download folder, a build tree or a
# USB stick works until that folder moves, and then `gleipnir` disappears with no
# obvious cause.  An installed program should not care what happened to the
# thing it was installed from.
#
# Per-user, so it needs no administrator rights and touches nothing outside
# your own profile.  For a machine-wide install, put gleipnir.exe in a directory of
# your choosing and add it to the system PATH yourself -- that needs elevation
# and is not something this script should do behind your back.
#
#   powershell -ExecutionPolicy Bypass -File install.ps1

$ErrorActionPreference = 'Stop'

$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$dest = Join-Path $env:LOCALAPPDATA 'Programs\gleipnir'

Write-Host "installing gleipnir"
Write-Host "  from: $src"
Write-Host "  to:   $dest"

if (-not (Test-Path (Join-Path $src 'gleipnir.exe'))) {
    throw "gleipnir.exe not found next to this script ($src)"
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null

# End-user files only.  ARCHITECTURE.md and EXPERIENCES.md are development
# notes and belong with the source, not in an install directory.
foreach ($f in @('gleipnir.exe', 'README.txt', 'USAGE.txt', 'uninstall.ps1')) {
    $p = Join-Path $src $f
    if (Test-Path $p) {
        Copy-Item $p -Destination $dest -Force
        Write-Host "  copied $f"
    }
}

# User PATH via the .NET API, never `setx PATH "%PATH%;..."`.  That expands to
# the combined machine+user PATH, copies the machine half into your user half,
# and then truncates the result at 1024 characters.
$old = [Environment]::GetEnvironmentVariable('Path', 'User')
$entries = $old -split ';' | Where-Object { $_ }

if ($entries -contains $dest) {
    Write-Host "  PATH already contains $dest"
} else {
    $bak = Join-Path $dest ("PATH-backup-" + (Get-Date -Format yyyyMMdd-HHmmss) + ".txt")
    $old | Out-File -FilePath $bak -Encoding utf8 -NoNewline
    [Environment]::SetEnvironmentVariable('Path', ($old.TrimEnd(';') + ';' + $dest), 'User')
    Write-Host "  added to user PATH (previous value saved to $bak)"
}

# Verify against the installed copy rather than whatever is on the current
# PATH, which in this session may still be stale.
$exe = Join-Path $dest 'gleipnir.exe'
Write-Host ""
& $exe --version
Write-Host ""
Write-Host "done.  Open a NEW terminal, then:  gleipnir --help"
Write-Host "to remove:  powershell -ExecutionPolicy Bypass -File `"$dest\uninstall.ps1`""
