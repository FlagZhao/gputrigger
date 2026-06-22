# GPUTrigger

GPUTrigger is the runtime driver of [RedSan](https://github.com/FlagZhao/redsan). Loaded via `LD_PRELOAD` as `libgputrigger.so`, it attaches the [`gpu-patch`](../gpu-patch) CUDA instrumentation to a running process via NVIDIA's compute-sanitizer patching API, and streams the resulting memory-access trace into [`redshow`](../redshow) for redundancy analysis.

This component is normally built and driven for you by the top-level `redsan` repo (`bin/install_release.sh`, `bin/gpupunk`) — the instructions below are for building/running it on its own.

## Dependencies

- CUDA Toolkit with `compute-sanitizer` (>= 11.2)
- A built `gpu-patch` checkout (for its headers)
- A built `redshow` static library (`libredshow.a`)
- `mbedtls` and `libelf` (e.g. via Spack)

## Build

```bash
mkdir build && cd build
cmake .. \
    -DCMAKE_INSTALL_PREFIX=<install_path>/gputrigger \
    -Dgpu_patch_path=<install_path>/gpu-patch \
    -Dredshow_path=<install_path>/redshow
make -j
make install
```

Pass `-DENABLE_DEBUG=ON` for a debug build (`-g -O0 -DDEBUG`).

## Usage

`libgputrigger.so` is controlled entirely through environment variables, normally set up by `setgpupunk.sh`/`gpupunk` in the top-level repo:

| Variable | Purpose |
|---|---|
| `GPUPATCH_PATH` | Path to the `gpu-patch` fatbin/install directory |
| `GPUPUNK_ANALYSIS_MODE` | Numeric `redshow_analysis_type_t` selecting the analysis mode (e.g. memory access, CCT, redundant write) |
| `GPUPUNK_WHITELIST` | Optional path to a kernel whitelist file |
| `GPUPUNK_SANITIZER_*` | Control knobs from `include/control-knob.h` (record/buffer sizes, sampling frequency, approximation level, whitelist/blacklist, etc.) |

Minimal manual run example:

```bash
export GPUPATCH_PATH=<install_path>/gpu-patch
export LD_LIBRARY_PATH=<install_path>/redshow/lib:$LD_LIBRARY_PATH
LD_PRELOAD=<install_path>/gputrigger/libgputrigger.so ./your_cuda_app
```
