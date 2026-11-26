<#
.SYNOPSIS
    Detect (and optionally fix) inconsistent VS2022 build artifacts.

.DESCRIPTION
    Diagnoses the failure modes that have caused silent corruption
    on Windows builds in the past, in particular the 2026-05-09
    incident where bin\RISE.lib went stale relative to its own .obj
    files and RISE-GUI.exe linked against the stale lib, producing
    a vtable mismatch with no compile / link error.

    Checks performed (per Configuration in -Configurations):
      * extlib\oidn\install\lib\OpenImageDenoise.lib exists
        (preferred to system OIDN — see CLAUDE.md "OIDN").
      * RISE.lib exists in the config's output dir.
      * RISE.lib is at least as new as the newest .obj in
        Library\<Config>\.  Older = stale, dependent .exes will
        link against the wrong vtable.
      * RISE-CLI.exe / RISE-GUI.exe exist and are at least as new
        as RISE.lib.  Older = those binaries pre-date the current
        lib, run them and you're testing yesterday's code.

    Default mode is report-only.  Pass -Fix to delete stale outputs
    + tracker files; the next VS Build Solution will then re-run
    Lib.exe and re-link the .exes against a fresh RISE.lib.

.PARAMETER Configurations
    One or both of "Release", "Debug".  Default: both.

.PARAMETER Fix
    Delete stale outputs (RISE.lib + Lib*.tlog + dependent .exes).
    Without this flag the script only reports.

.EXAMPLE
    pwsh -File tools\build-doctor.ps1
    pwsh -File tools\build-doctor.ps1 -Fix
    pwsh -File tools\build-doctor.ps1 -Configurations Release -Fix
#>

[CmdletBinding()]
param(
	[ValidateSet('Release', 'Debug')]
	[string[]]$Configurations = @('Release', 'Debug'),

	[switch]$Fix
)

$ErrorActionPreference = 'Stop'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot  = Resolve-Path (Join-Path $ScriptDir '..')

function Get-LatestMtime {
	param([string]$Path, [string]$Filter)
	if (-not (Test-Path $Path)) { return $null }
	$item = Get-ChildItem -Path $Path -Filter $Filter -ErrorAction SilentlyContinue |
		Sort-Object LastWriteTime -Descending |
		Select-Object -First 1
	return $item
}

function Format-Mtime {
	param($Item)
	if (-not $Item) { return '<missing>' }
	return $Item.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
}

$findings = @()

function Add-Finding {
	param([string]$Severity, [string]$Message, [string[]]$FixPaths = @())
	$script:findings += [pscustomobject]@{
		Severity = $Severity
		Message  = $Message
		FixPaths = $FixPaths
	}
}

# OIDN install — same set the in-tree guard checks.
$oidnLib = Join-Path $RepoRoot 'extlib\oidn\install\lib\OpenImageDenoise.lib'
if (-not (Test-Path $oidnLib)) {
	$oidnBuild = Join-Path $RepoRoot 'extlib\oidn\source\build\Release\OpenImageDenoise.lib'
	if (Test-Path $oidnBuild) {
		Add-Finding 'WARN' "OIDN install/ tree is missing but source/build/Release/ has fresh artifacts.  Run cmake's install step from extlib\oidn\source\build, or pwsh -File extlib\oidn\build.ps1."
	} else {
		Add-Finding 'ERROR' "OIDN install/ AND source/build/ are both missing.  Run pwsh -File extlib\oidn\build.ps1 from repo root (see CLAUDE.md `"OIDN`")."
	}
} else {
	Write-Host ("[ ok ] OIDN install lib   {0}" -f (Format-Mtime (Get-Item $oidnLib)))
}

foreach ($cfg in $Configurations) {
	$outDir = if ($cfg -eq 'Release') { 'bin' } else { 'dbin' }
	$outPath = Join-Path $RepoRoot $outDir
	$objDir  = Join-Path $RepoRoot ("build\VS2022\Library\{0}" -f $cfg)
	$tlogDir = Join-Path $objDir 'Library.tlog'

	$riseLibPath = Join-Path $outPath 'RISE.lib'
	$cliPath     = Join-Path $outPath 'RISE-CLI.exe'
	$guiPath     = Join-Path $outPath 'RISE-GUI.exe'

	Write-Host ("--- {0} ---" -f $cfg)

	$riseLib = if (Test-Path $riseLibPath) { Get-Item $riseLibPath } else { $null }
	$cli     = if (Test-Path $cliPath)     { Get-Item $cliPath }     else { $null }
	$gui     = if (Test-Path $guiPath)     { Get-Item $guiPath }     else { $null }
	$newestObj = Get-LatestMtime -Path $objDir -Filter '*.obj'

	Write-Host ("       RISE.lib       {0}" -f (Format-Mtime $riseLib))
	Write-Host ("       newest .obj    {0}" -f (Format-Mtime $newestObj))
	Write-Host ("       RISE-CLI.exe   {0}" -f (Format-Mtime $cli))
	Write-Host ("       RISE-GUI.exe   {0}" -f (Format-Mtime $gui))

	# .obj files exist but no RISE.lib — Lib.exe was skipped.
	if ($newestObj -and -not $riseLib) {
		Add-Finding 'ERROR' ("[{0}] {1}\RISE.lib is missing but Library\{0}\ has fresh .obj files.  Lib.exe was skipped — Build Solution to relink." -f $cfg, $outDir) `
			@()
		continue
	}

	# RISE.lib older than newest .obj — silent-corruption case.
	if ($riseLib -and $newestObj -and $newestObj.LastWriteTime -gt $riseLib.LastWriteTime) {
		$staleBy = $newestObj.LastWriteTime - $riseLib.LastWriteTime
		$msg = "[{0}] {1}\RISE.lib is STALE — newest .obj ({2}) is {3:N0}s newer.  Dependent .exes link against wrong vtable.  Delete RISE.lib + Lib*.tlog and rebuild." `
			-f $cfg, $outDir, $newestObj.Name, $staleBy.TotalSeconds
		$fixSet = @($riseLibPath)
		if (Test-Path $tlogDir) {
			$fixSet += @(Get-ChildItem -Path $tlogDir -Filter 'Lib*.tlog' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
		}
		# .exes that pre-date the (stale) lib will be replaced once
		# Lib re-runs and produces a fresh RISE.lib; but if the .exes
		# are NEWER than the lib, they were linked against the stale
		# lib and need to be re-linked.
		if ($cli -and $cli.LastWriteTime -gt $riseLib.LastWriteTime) { $fixSet += $cliPath }
		if ($gui -and $gui.LastWriteTime -gt $riseLib.LastWriteTime) { $fixSet += $guiPath }
		Add-Finding 'ERROR' $msg $fixSet
		continue
	}

	# .exes older than RISE.lib — pre-date the lib, won't reflect latest changes.
	$staleExes = @()
	if ($cli -and $riseLib -and $cli.LastWriteTime -lt $riseLib.LastWriteTime) { $staleExes += 'RISE-CLI.exe' }
	if ($gui -and $riseLib -and $gui.LastWriteTime -lt $riseLib.LastWriteTime) { $staleExes += 'RISE-GUI.exe' }
	if ($staleExes.Count -gt 0) {
		Add-Finding 'WARN' ("[{0}] {1} predate RISE.lib — Build Solution to relink so you're not testing stale binaries." -f $cfg, ($staleExes -join ' / ')) @()
	}
}

Write-Host
if ($findings.Count -eq 0) {
	Write-Host '==> All build artifacts are consistent.'
	exit 0
}

Write-Host '==> Findings:'
foreach ($f in $findings) {
	$prefix = if ($f.Severity -eq 'ERROR') { '[FAIL]' } else { '[WARN]' }
	Write-Host ("{0} {1}" -f $prefix, $f.Message)
}

$errors = $findings | Where-Object { $_.Severity -eq 'ERROR' }
if (-not $Fix) {
	Write-Host
	Write-Host 'Run with -Fix to delete stale outputs.  Then Build Solution in VS2022.'
	if ($errors.Count -gt 0) { exit 1 }
	exit 0
}

# -Fix: delete stale paths.
$toDelete = @()
foreach ($f in $findings) { $toDelete += $f.FixPaths }
$toDelete = $toDelete | Where-Object { $_ -and (Test-Path $_) } | Sort-Object -Unique

if ($toDelete.Count -eq 0) {
	Write-Host
	Write-Host '==> Nothing to delete.'
	exit 0
}

Write-Host
Write-Host '==> Deleting stale outputs:'
foreach ($p in $toDelete) {
	Write-Host ("       {0}" -f $p)
	Remove-Item -Force $p
}

Write-Host
Write-Host '==> Done.  Now run Build > Build Solution in VS2022.'
