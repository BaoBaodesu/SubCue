param(
    [string]$Python = "C:\Users\baobao\AppData\Roaming\uv\python\cpython-3.12-windows-x86_64-none\python.exe"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$environment = Join-Path $root ".venv-inference"
$cache = Join-Path $root ".uv-cache"
$packages = Join-Path $root "out\inference-packages"
$torchWheel = Join-Path $packages "torch-2.6.0+cu124-cp312-cp312-win_amd64.whl"
$env:UV_CACHE_DIR = $cache

uv venv $environment --python $Python
New-Item -ItemType Directory -Force -Path $packages | Out-Null
curl.exe -L --fail --retry 20 --retry-all-errors --retry-delay 2 -C - `
    -o $torchWheel `
    "https://download-r2.pytorch.org/whl/cu124/torch-2.6.0%2Bcu124-cp312-cp312-win_amd64.whl"
if ((Get-FileHash -Algorithm SHA256 $torchWheel).Hash -ne
    "3313061C1FEC4C7310CF47944E84513DCD27B6173B72A349BB7CA68D0EE6E9C0") {
    throw "CUDA PyTorch wheel 校验失败"
}
uv pip install --python (Join-Path $environment "Scripts\python.exe") $torchWheel
uv pip install --python (Join-Path $environment "Scripts\python.exe") "qwen-asr==0.0.6"

& (Join-Path $environment "Scripts\python.exe") -c @'
import torch
assert torch.cuda.is_available(), "CUDA 不可用"
x = torch.arange(1024, device="cuda")
print(f"torch={torch.__version__} cuda={torch.version.cuda} device={torch.cuda.get_device_name(0)} sum={x.sum().item()}")
'@
