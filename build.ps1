# Build the visualizer on Windows: the PowerShell equivalent of build.sh, beside it.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# Needs a MinGW-w64 g++ on PATH (WinLibs: winget install BrechtSanders.WinLibs.POSIX.UCRT) and
# oni-sim-replacement cloned beside this repository and built once (its sim\build.ps1 generates
# abi\sim_abi.h). ONI_SIM_REPO overrides where that repository is.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
# ---------------------------------------------------------------- helpers (Windows PowerShell 5.1)

function Fail([string]$Message) {
  [Console]::Error.WriteLine("build.ps1: $Message")
  exit 1
}

# Run a native program, passing its stdout through and its stderr to stderr as plain text, and
# stop on a non-zero exit code. ErrorActionPreference is relaxed for the call itself because
# Windows PowerShell 5.1 turns a native program's stderr lines into errors when output is
# redirected, which would stop the build at the first compiler warning.
function Invoke-Native([string]$What, [string]$Exe, [string[]]$Arguments) {
  $saved = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & $Exe @Arguments 2>&1 | ForEach-Object {
      if ($_ -is [System.Management.Automation.ErrorRecord]) { [Console]::Error.WriteLine($_.ToString()) }
      else { $_ }
    }
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $saved
  }
  if ($code -ne 0) { Fail "$What failed (exit code $code)" }
}

# git output, or $null when git is missing or the command fails (not a checkout, a shallow
# clone, ...). The version stamp degrades to literals rather than stopping the build.
function Get-GitOutput([string[]]$GitArgs) {
  if (-not (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)) { return $null }
  $saved = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $out = & git @GitArgs 2>$null
    $code = $LASTEXITCODE
  } catch {
    $code = 1
  } finally {
    $ErrorActionPreference = $saved
  }
  if ($code -ne 0) { return $null }
  return ((@($out) | ForEach-Object { "$_" }) -join "`n").Trim()
}

# MAJOR.MINOR.REV+SHA[*]: every non-comment line of the VERSION file with all whitespace removed,
# the commit count and short commit of HEAD, and '*' if the working tree has any change.
function Get-VersionParts([string]$VersionFile) {
  $base = ((Get-Content -LiteralPath $VersionFile | Where-Object { $_ -notmatch '^#' }) -join '') -replace '\s', ''
  $rev = Get-GitOutput @('rev-list', '--count', 'HEAD')
  if (-not $rev) { $rev = '0' }
  $sha = Get-GitOutput @('rev-parse', '--short=7', 'HEAD')
  if (-not $sha) { $sha = 'unknown' }
  $dirty = ''
  if (Get-GitOutput @('status', '--porcelain')) { $dirty = '*' }
  return @{ Base = $base; Rev = $rev; Sha = $sha; Dirty = $dirty }
}

# Write text exactly: UTF-8 without a byte order mark, and only the line endings in $Text.
function Write-Exact([string]$Path, [string]$Text) {
  $full = Join-Path (Get-Location).ProviderPath $Path
  [System.IO.File]::WriteAllText($full, $Text, (New-Object System.Text.UTF8Encoding $false))
}
# A MinGW-w64 g++ that targets 64-bit Windows: x86_64-w64-mingw32-g++ if present, else g++.
function Find-Gxx {
  foreach ($name in @('x86_64-w64-mingw32-g++', 'g++')) {
    $c = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $c) { continue }
    $m = $null
    $code = 1
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $m = & $c.Source -dumpmachine 2>$null; $code = $LASTEXITCODE } finally { $ErrorActionPreference = $saved }
    if ($code -eq 0 -and "$m" -match '^x86_64-.*mingw') { return $c.Source }
    Fail ("$($c.Source) targets '$m', not 64-bit Windows (x86_64-w64-mingw32). Install WinLibs:  " +
          "winget install BrechtSanders.WinLibs.POSIX.UCRT  and put its mingw64\bin first on PATH.")
  }
  Fail ("No g++ on PATH. Install WinLibs:  winget install BrechtSanders.WinLibs.POSIX.UCRT  and " +
        "open a new PowerShell window.")
}

# Hand the arguments to g++ in a response file (@file), so quotes and spaces reach the compiler
# unchanged: Windows PowerShell 5.1 strips the quotes inside -DNAME="value" on a command line.
function Invoke-Gxx([string]$Gxx, [string]$ResponseFile, [string[]]$Arguments) {
  $lines = foreach ($a in $Arguments) { '"' + (($a -replace '\\', '\\') -replace '"', '\"') + '"' }
  Write-Exact $ResponseFile (($lines -join "`n") + "`n")
  Invoke-Native 'g++' $Gxx @('@' + $ResponseFile)
}

# ---------------------------------------------------------------- build

Push-Location -LiteralPath $PSScriptRoot
try {
  New-Item -ItemType Directory -Force -Path build | Out-Null

  # The ABI header and the offline host plumbing belong to oni-sim-replacement and are reached on
  # the include path, not vendored here.
  $simRepo = $env:ONI_SIM_REPO
  if (-not $simRepo) { $simRepo = '..\oni-sim-replacement' }
  $simRepo = $simRepo.TrimEnd('\', '/')
  if (-not (Test-Path -LiteralPath (Join-Path $simRepo 'driver\src\simhost.h') -PathType Leaf)) {
    Fail ("cannot find oni-sim-replacement at '$simRepo'. Clone it beside this repo, or set " +
          "`$env:ONI_SIM_REPO to the path of your oni-sim-replacement clone.")
  }
  if (-not (Test-Path -LiteralPath (Join-Path $simRepo 'abi\sim_abi.h') -PathType Leaf)) {
    Fail ("'$simRepo\abi\sim_abi.h' is missing: build oni-sim-replacement first " +
          "(sim\build.ps1 -OniGame <your Oxygen Not Included folder>, in that repo).")
  }
  $inc = (Resolve-Path -LiteralPath $simRepo).ProviderPath -replace '\\', '/'

  # The flags of build.sh (-O2: this binary never times the sim's kernels), plus -static, so
  # simviz.exe does not need the WinLibs libwinpthread-1.dll to start.
  $gxx = Find-Gxx
  $gxxArgs = @('-O2', '-std=c++17', '-static-libgcc', '-static-libstdc++', '-static',
               '-Wall', '-Wextra', '-Wno-cast-function-type',
               "-I$inc/driver/src",
               "-I$inc",
               '-o', 'build/simviz.exe', 'src/main.cpp', '-lgdi32', '-lcomdlg32', '-lws2_32')
  Invoke-Gxx $gxx 'build\gcc-args.rsp' $gxxArgs

  Write-Output 'built build/simviz.exe'
} finally {
  Pop-Location
}
