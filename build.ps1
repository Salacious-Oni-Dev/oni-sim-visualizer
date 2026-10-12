# Build the visualizer on Windows: the PowerShell equivalent of build.sh, beside it.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# Needs Qt 6.8.3 for MinGW and the MinGW 13.1 compiler Qt builds it with (Qt's own toolchain, so
# the compiler and the Qt DLLs agree on the C++ runtime and on POSIX threads). Either the Qt
# online installer (Qt 6.8.3 > MinGW 13.1.0 64-bit, and Developer and Designer Tools > MinGW
# 13.1.0 64-bit), or aqtinstall:
#
#   py -m pip install aqtinstall
#   py -m aqt install-qt windows desktop 6.8.3 win64_mingw -O C:\Qt
#   py -m aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 -O C:\Qt
#
# ONI_QT overrides where Qt is (default C:\Qt\6.8.3\mingw_64). The compiler is taken from
# <Qt root>\Tools\mingw1310_64\bin when it is there, else the first g++ on PATH.
#
# Also needs oni-sim-replacement cloned beside this repository and built once (its
# sim\build.ps1 generates abi\sim_abi.h). ONI_SIM_REPO overrides where that repository is.
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
# A MinGW-w64 g++ that targets 64-bit Windows with POSIX threads: Qt's own MinGW under
# $ToolsBin if it is there, else x86_64-w64-mingw32-g++ or g++ on PATH.
function Find-Gxx([string]$ToolsBin) {
  $candidates = @()
  $own = Join-Path $ToolsBin 'g++.exe'
  if (Test-Path -LiteralPath $own -PathType Leaf) { $candidates += $own }
  foreach ($name in @('x86_64-w64-mingw32-g++', 'g++')) {
    $c = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { $candidates += $c.Source }
  }
  if ($candidates.Count -eq 0) {
    Fail ("No g++ found in '$ToolsBin' or on PATH. Install Qt's MinGW 13.1 as the comment at the " +
          "top of this script says.")
  }
  $gxx = $candidates[0]
  $m = $null
  $v = $null
  $code = 1
  $saved = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    $m = & $gxx -dumpmachine 2>$null
    $code = $LASTEXITCODE
    $v = (& $gxx -v 2>&1 | ForEach-Object { "$_" }) -join "`n"
  } finally { $ErrorActionPreference = $saved }
  if ($code -ne 0 -or "$m" -notmatch '^x86_64-.*mingw') {
    Fail "$gxx targets '$m', not 64-bit Windows (x86_64-w64-mingw32). Use Qt's MinGW 13.1."
  }
  if ($v -notmatch 'Thread model: posix') {
    Fail ("$gxx is not a POSIX-threads MinGW, and Qt's headers need one (they use both std::thread " +
          "and the pthread runtime). Use Qt's MinGW 13.1.")
  }
  return $gxx
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

  # Qt 6 (LGPLv3, linked dynamically) is the UI toolkit (see the README).
  $qtVersion = '6.8.3'
  $qt = $env:ONI_QT
  if (-not $qt) { $qt = "C:\Qt\$qtVersion\mingw_64" }
  $qt = $qt.TrimEnd('\', '/')
  if (-not (Test-Path -LiteralPath (Join-Path $qt 'include\QtWidgets\QApplication') -PathType Leaf)) {
    Fail ("cannot find Qt $qtVersion (MinGW 64-bit) at '$qt'. Install it as the comment at the top " +
          "of this script says, or set `$env:ONI_QT to its mingw_64 folder.")
  }
  $qt = (Resolve-Path -LiteralPath $qt).ProviderPath
  $qtInc = $qt -replace '\\', '/'
  $toolsBin = Join-Path (Split-Path -Parent (Split-Path -Parent $qt)) 'Tools\mingw1310_64\bin'

  # The flags of build.sh: -O2 (this binary never times the sim's kernels), our own code with
  # libgcc/libstdc++ linked statically, Qt and its MinGW runtime as DLLs beside the exe. No moc:
  # signals are wired with functor connects only, so this stays one compiler line.
  $gxx = Find-Gxx $toolsBin
  $gxxArgs = @('-O2', '-std=c++17', '-static-libgcc', '-static-libstdc++',
               '-Wall', '-Wextra', '-Wno-cast-function-type',
               "-I$inc/driver/src",
               "-I$inc",
               '-isystem', "$qtInc/include", '-isystem', "$qtInc/include/QtCore",
               '-isystem', "$qtInc/include/QtGui", '-isystem', "$qtInc/include/QtWidgets",
               '-o', 'build/simviz.exe', 'src/main.cpp',
               "-L$qtInc/lib", '-lQt6Widgets', '-lQt6Gui', '-lQt6Core',
               '-lws2_32', '-Wl,-Bstatic', '-lwinpthread', '-Wl,-Bdynamic')
  Invoke-Gxx $gxx 'build\gcc-args.rsp' $gxxArgs

  # The runtime the exe needs beside it, as build.sh copies it: the three Qt modules, the MinGW
  # runtime Qt links against, and the windows and offscreen platform plugins.
  New-Item -ItemType Directory -Force -Path build\platforms | Out-Null
  foreach ($f in @('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'libgcc_s_seh-1.dll',
                   'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $qt "bin\$f") -Destination build -Force
  }
  foreach ($f in @('qwindows.dll', 'qoffscreen.dll')) {
    Copy-Item -LiteralPath (Join-Path $qt "plugins\platforms\$f") -Destination build\platforms -Force
  }

  Write-Output "built build/simviz.exe (Qt $qtVersion)"
} finally {
  Pop-Location
}
