$ErrorActionPreference = "Stop"

$repository = $PSScriptRoot
$image = "lumahome-toolchain:1"

docker info | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Docker is not running or this account cannot access the Docker daemon. Start Docker Desktop and retry."
}

docker build --tag $image --file (Join-Path $repository "Dockerfile") $repository
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

docker run --rm `
    --volume "${repository}:/project" `
    --workdir /project `
    $image `
    make -j2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$firmware = Join-Path $repository "boot.firm"
if (-not (Test-Path -LiteralPath $firmware)) {
    throw "The build completed without producing boot.firm."
}

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $firmware).Hash
Write-Host "Built: $firmware"
Write-Host "SHA-256: $hash"
