RPCS3
=====

[![GitHub Actions](https://img.shields.io/github/actions/workflow/status/RPCS3/rpcs3/rpcs3.yml?branch=master&logo=github&label=Actions)](https://github.com/RPCS3/rpcs3/actions/workflows/rpcs3.yml)
[![RPCS3 Discord Server](https://img.shields.io/discord/272035812277878785?color=5865F2&label=RPCS3%20Discord&logo=discord&logoColor=white)](https://discord.gg/rpcs3)

The world's first free and open-source PlayStation 3 emulator/debugger, written in C++ for Windows, Linux, macOS and FreeBSD.

You can find some basic information on our [**website**](https://rpcs3.net/). Game info is being populated on the [**Wiki**](https://wiki.rpcs3.net/).
For discussion about this emulator, PS3 emulation, and game compatibility reports, please visit our [**forums**](https://forums.rpcs3.net) and our [**Discord server**](https://discord.gg/RPCS3).

[**Support Lead Developers Nekotekina and kd-11 on Patreon**](https://www.patreon.com/Nekotekina)

## Contributing

If you want to help the project but do not code, the best way to help out is to test games and make bug reports. See:
* [Quickstart](https://rpcs3.net/quickstart)

If you want to contribute as a developer, please take a look at the following pages:

* [Coding Style](https://github.com/RPCS3/rpcs3/wiki/Coding-Style)
* [Developer Information](https://github.com/RPCS3/rpcs3/wiki/Developer-Information)

You should also contact any of the developers in the forums or in the Discord server to learn more about the current state of the emulator.

## Building

See [BUILDING.md](BUILDING.md) for more information about how to setup an environment to build RPCS3.

## Running

Check our friendly [quickstart](https://rpcs3.net/quickstart) guide to make sure your computer meets the minimum system requirements to run RPCS3.

Don't forget to have your graphics driver up to date and to install the [Visual C++ Redistributable Packages for Visual Studio 2022](https://aka.ms/vs/17/release/VC_redist.x64.exe) if you are a Windows user.

## License

Most files are licensed under the terms of GNU GPL-2.0-only License; see LICENSE file for details. Some files may be licensed differently; check appropriate file headers for details.




# BUILDING DX9 Version

Ecco la guida completa per buildare RPCS3 con backend DX9 da zero:

---

## Build RPCS3-DX9 da zero (Windows)

### Prerequisiti

1. **Visual Studio 2022** con workload "Desktop development with C++"
2. **Qt 6.10.1** — msvc2022_64
3. **Vulkan SDK** — 1.4.328.1
4. **Git** (con submodules già inizializzati)

### Step 1 — Clona e inizializza i submodules

```powershell
git clone <repo-url> rpcs3
cd rpcs3
git submodule update --init --recursive
```

### Step 2 — Scarica le LLVM precompiled libs

```powershell
# Crea la directory
New-Item -Path "build\lib_ext\Release-x64" -ItemType Directory -Force

# Scarica
Invoke-WebRequest -Uri "https://github.com/RPCS3/llvm-mirror/releases/download/custom-build-win-19.1.7/llvmlibs_mt.7z" -OutFile "build\lib_ext\llvmlibs_mt.7z" -UseBasicParsing

# Estrai (richiede 7-Zip)
& "C:\Program Files\7-Zip\7z.exe" x "build\lib_ext\llvmlibs_mt.7z" -o"build\lib_ext\Release-x64" -y
```

### Step 3 — Configura environment e builda

```powershell
$env:QTDIR = "C:\Qt\6.10.1\msvc2022_64"

& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
    rpcs3.sln `
    /p:Configuration=Release `
    /p:Platform=x64 `
    /p:VcpkgEnabled=false `
    /m `
    /v:minimal
```

### Step 4 — Output

L'eseguibile sarà in: `bin\rpcs3.exe`

---

### Note importanti

| Parametro | Motivo |
|---|---|
| `/p:VcpkgEnabled=false` | **Obbligatorio** — vcpkg system integration causa conflitti LNK2005 con protobuf |
| `/m` | Build parallelo (usa tutti i core) |
| `Release` | Config raccomandata. Per Debug servono anche le LLVM debug libs (non disponibili precompilate) |

Se vuoi buildare da Visual Studio (GUI) invece che da riga di comando:
1. Apri rpcs3.sln in VS2022
2. Imposta `QTDIR` come variabile di ambiente di sistema prima di aprire VS
3. Vai in **Tools → Options → vcpkg → Enable vcpkg → OFF**  
4. Seleziona **Release | x64**
5. **Build → Build Solution**