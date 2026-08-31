# Reference codecs on the full Silesia corpus: size, compression and
# decompression wall time, and peak working set.
#
# gen reports its own peak RSS from inside the process, which is exact.  The
# reference codecs do not, and PeakWorkingSet64 read after exit returns zero on
# this machine, so their memory is *sampled* while the process runs and is
# therefore a lower bound.  Labelled as sampled wherever it is reported.
#
# Run with nothing else on the machine, and never at the same time as
# bench_final.py -- a concurrent job once produced a phantom 8% regression in
# this project's history.
#
#   powershell -File bench_refs.ps1          # writes bench_refs.json

# repo root: this script lives in scripts/, so climb one more level
$here = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$sil  = Join-Path $here "tools\corpora\silesia"
$tmp  = Join-Path $env:TEMP "refbench"
if (-not (Test-Path $tmp)) { New-Item -ItemType Directory $tmp | Out-Null }

$files = @("xml","ooffice","reymont","sao","x-ray","mr","osdb","dickens",
           "samba","nci","webster","mozilla")

$all = @{}

function Run-Sampled([string]$exe, [string]$argline, [string]$expect) {
  if (Test-Path $expect) { Remove-Item $expect -Force -Recurse }
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $exe
  $psi.Arguments = $argline
  $psi.UseShellExecute = $false
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $p = [System.Diagnostics.Process]::Start($psi)
  $peak = 0
  while (-not $p.HasExited) {
    try { $p.Refresh(); if ($p.WorkingSet64 -gt $peak) { $peak = $p.WorkingSet64 } } catch {}
    Start-Sleep -Milliseconds 50
  }
  $p.WaitForExit()
  $sw.Stop()
  $p.StandardOutput.ReadToEnd() | Out-Null
  $p.StandardError.ReadToEnd()  | Out-Null
  $p.Dispose()
  # A blocked or failed run leaves no output; the caller must not treat that as
  # a datapoint.  (Application Control on this machine blocks binaries by hash.)
  $ok = (Test-Path $expect)
  return @{ secs = $sw.Elapsed.TotalSeconds; peak = $peak; ok = $ok }
}

function Bench([string]$name, [scriptblock]$comp, [scriptblock]$decomp) {
  $tot = 0; $tc = 0.0; $td = 0.0; $mem = 0
  $rows = @{}
  foreach ($f in $files) {
    $src = Join-Path $sil $f
    if (-not (Test-Path $src)) { continue }
    $r = & $comp $src
    if ($null -eq $r -or -not $r.ok) {
      Write-Output ("{0,-14} FAILED on {1}" -f $name, $f); return
    }
    $d = & $decomp $src
    $dsecs = 0.0
    if ($null -ne $d -and $d.ok) {
      $dsecs = $d.secs
      $td += $d.secs
      if ($d.peak -gt $mem) { $mem = $d.peak }
    }
    $tot += $r.size; $tc += $r.secs
    if ($r.peak -gt $mem) { $mem = $r.peak }
    $rows[$f] = @{ size = $r.size; ctime = $r.secs; dtime = $dsecs
                   cpeak = $r.peak; insize = (Get-Item $src).Length }
    Write-Output ("  {0,-9}{1,11:N0}{2,9:F1}s{3,9:F1}s{4,8:N0} MB" -f `
                  $f, $r.size, $r.secs, $dsecs, ($r.peak / 1MB))
  }
  Write-Output ("{0,-14}{1,13:N0}{2,10:F1}s{3,10:F1}s{4,9:N0} MB" -f `
                $name, $tot, $tc, $td, ($mem / 1MB))
  Write-Output ""
  $all[$name] = @{ total = $tot; ctime = $tc; dtime = $td; peak = $mem
                   rows = $rows }
}

Write-Output ("{0,-14}{1,13}{2,11}{3,11}{4,12}" -f "codec","total","comp","decomp","peak (sampled)")
Write-Output ""

$zpaq = Join-Path $here "tools\zpaq\zpaq64.exe"
Bench "zpaq -m5" {
  param($src)
  $c = Join-Path $tmp "a.zpaq"
  $r = Run-Sampled $zpaq ("a `"$c`" `"$src`" -m5 -t1") $c
  if ($r.ok) { $r.size = (Get-Item $c).Length }
  $r
} {
  param($src)
  $c = Join-Path $tmp "a.zpaq"; $d = Join-Path $tmp "zx"
  $r = Run-Sampled $zpaq ("x `"$c`" -to `"$d`" -t1") $d
  Remove-Item $c -Force -ErrorAction SilentlyContinue
  Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue
  $r
}

$lpaq = Join-Path $here "tools\lpaq1.exe"
Bench "lpaq1 -6" {
  param($src)
  $c = Join-Path $tmp "a.lpq"
  $r = Run-Sampled $lpaq ("6 `"$src`" `"$c`"") $c
  if ($r.ok) { $r.size = (Get-Item $c).Length }
  $r
} {
  param($src)
  $c = Join-Path $tmp "a.lpq"; $o = Join-Path $tmp "a.out"
  $r = Run-Sampled $lpaq ("d `"$c`" `"$o`"") $o
  Remove-Item $c,$o -Force -ErrorAction SilentlyContinue
  $r
}

# xz and bzip2 write beside their input rather than to stdout here, because
# Run-Sampled redirects stdout to a pipe and `-c` would send the archive into it.
Bench "xz -9e" {
  param($src)
  $work = Join-Path $tmp "work.bin"
  Copy-Item $src $work -Force
  $c = "$work.xz"
  $r = Run-Sampled "xz" ("-9e -T1 -f `"$work`"") $c
  if ($r.ok) { $r.size = (Get-Item $c).Length }
  $r
} {
  param($src)
  $work = Join-Path $tmp "work.bin"
  $c = "$work.xz"
  $r = Run-Sampled "xz" ("-d -T1 -f `"$c`"") $work
  Remove-Item $work,$c -Force -ErrorAction SilentlyContinue
  $r
}

Bench "brotli -q11" {
  param($src)
  $c = Join-Path $tmp "a.br"
  $r = Run-Sampled "brotli" ("-q 11 -w 24 -f -o `"$c`" `"$src`"") $c
  if ($r.ok) { $r.size = (Get-Item $c).Length }
  $r
} {
  param($src)
  $c = Join-Path $tmp "a.br"; $o = Join-Path $tmp "a.out"
  $r = Run-Sampled "brotli" ("-d -f -o `"$o`" `"$c`"") $o
  Remove-Item $c,$o -Force -ErrorAction SilentlyContinue
  $r
}

Bench "bzip2 -9" {
  param($src)
  $work = Join-Path $tmp "work.bin"
  Copy-Item $src $work -Force
  $c = "$work.bz2"
  $r = Run-Sampled "bzip2" ("-9 -f `"$work`"") $c
  if ($r.ok) { $r.size = (Get-Item $c).Length }
  $r
} {
  param($src)
  $work = Join-Path $tmp "work.bin"
  $c = "$work.bz2"
  $r = Run-Sampled "bzip2" ("-d -f `"$c`"") $work
  Remove-Item $work,$c -Force -ErrorAction SilentlyContinue
  $r
}

Bench "gzip -9" {
  param($src)
  $work = Join-Path $tmp "work.bin"
  Copy-Item $src $work -Force
  $c = "$work.gz"
  $r = Run-Sampled "gzip" ("-9 -f `"$work`"") $c
  if ($r.ok) { $r.size = (Get-Item $c).Length }
  $r
} {
  param($src)
  $work = Join-Path $tmp "work.bin"
  $c = "$work.gz"
  $r = Run-Sampled "gzip" ("-d -f `"$c`"") $work
  Remove-Item $work,$c -Force -ErrorAction SilentlyContinue
  $r
}

$out = Join-Path $here "bench_refs.json"
$all | ConvertTo-Json -Depth 6 | Out-File -FilePath $out -Encoding utf8
Write-Output "wrote bench_refs.json"
