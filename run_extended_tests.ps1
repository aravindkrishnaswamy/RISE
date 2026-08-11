param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binName = if ($Config -eq "Debug") { "dbin" } else { "bin" }
$binDir = Join-Path $repoRoot "$binName/tests"
$testRunner = Join-Path $repoRoot "run_all_tests.ps1"
$extendedTests = @("AutoRasterizerTest", "PathTracingThermalEmissionTest")

function Invoke-ExtendedTest {
    param([string]$Name, [string[]]$Arguments)
    $testPath = Join-Path $binDir "$Name.exe"
    if (-not (Test-Path $testPath)) {
        throw "Missing $testPath; build the rise_all_tests target first."
    }
    Write-Host "=== $Name $Arguments ==="
    & $testPath @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE" }
}

# Render-heavy experiments stay sequential: each render already consumes the
# available worker pool.
Push-Location $repoRoot
try {
    & $testRunner -Config $Config -BuildOnly -Filter $extendedTests
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to build current extended-test binaries."
    }
    Invoke-ExtendedTest "AutoRasterizerTest" @("--extended-fire-ablation", "--fire-preview-only")
    Invoke-ExtendedTest "PathTracingThermalEmissionTest" @("--extended-matrix")
}
finally {
    Pop-Location
}
