param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binName = if ($Config -eq "Debug") { "dbin" } else { "bin" }
$binDir = Join-Path $repoRoot "$binName/tests"

function Invoke-ExtendedTest {
    param([string]$Name, [string[]]$Arguments)
    $testPath = Join-Path $binDir "$Name.exe"
    if (-not (Test-Path $testPath)) {
        throw "Missing $testPath; build the rise_all_tests target first."
    }
    Write-Host "=== $Name $Arguments ==="
    & $testPath @Arguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

# Render-heavy experiments stay sequential: each render already consumes the
# available worker pool.
Invoke-ExtendedTest "AutoRasterizerTest" @("--extended-fire-ablation")
Invoke-ExtendedTest "PathTracingThermalEmissionTest" @("--extended-matrix")
