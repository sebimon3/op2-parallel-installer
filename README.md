# OP-2.2 Parallel Installer
> [!CAUTION]
> This was entirely vibecoded. The goal was simply to speed up the installation process, addon selection and installation should work properly, however i have not tested every possible selection case. The generated code may be laughably bad in some cases, i will most likely not merge any changes to improve code quality. If theres a genuine issue, please file one or submit a PR.

Drop-in replacement for the OP-2.2 Installer which multi-threads decompression and verification.

Measured on a Ryzen 7 5700X3D (8C/16T, 32 GiB, Windows 11 x64):

| Run | Time | Result |
|---|---|---|
| Original single-threaded installer | 834.25 s | baseline |
| Multi-threaded installer | ~80–85 s | 59/59 SHA-1, 55 files OK |

Actual speed improvement heavily depends on the number of CPU cores and to a lesser extent, hard drive speed.
## Use

1. Copy `op-2-2-mt.exe` into the folder that contains the ten original payload files (`op-2.2-1a.bin` … `op-2.2-2d.bin`).
2. Run


## Build

Requirements: Windows, Visual Studio Build Tools with the **Desktop development with C++** workload, Python 3.
1. Extract metadata from the original Inno Setup installer: `python reference\build_meta.py path\to\original\op-2.2.exe`
2. Generate the installer manifest: `python src\gen_manifest.py [path\to\op22_meta.json]`
3. Build `src\build.bat`
4. Output lands in `dist`
## How it works
The chunk manifest from the original Inno Setup installer was extracted and wired into this installer. That's it. If the original setup is ever updated, the new manifest can be regenerated like so (needs Python 3)
`python reference\build_meta.py path\to\original\op-2.2.exe   # -> reference/op22_meta.json`

`python src\gen_manifest.py [path\to\op22_meta.json]`
## Licensing

The C/Python code in this repository is provided under the MIT License (see `LICENSE`). The bundled LZMA SDK decoder is public domain; see `THIRD_PARTY_NOTICES.md`. The OP-2.2 payload files and artwork are not included in this repository and remain the property of their respective owners.
