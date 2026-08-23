# Building LumaHome with Docker

Docker provides the pinned devkitARM, libctru, `firmtool`, and `makerom`
versions used by this branch. You do not need to install the 3DS SDK directly
on the host computer.

## 1. Install the prerequisites

Install:

- [Git](https://git-scm.com/downloads)
- [Docker Desktop](https://www.docker.com/products/docker-desktop/) on Windows
  or macOS, or Docker Engine on Linux

On Windows, accept Docker Desktop's WSL 2 option when prompted. Reboot if its
installer requests it. Start Docker Desktop and wait until it reports that the
engine is running.

Verify both programs in PowerShell or a terminal:

```text
git --version
docker --version
docker info
```

`docker info` must display server information. A named-pipe or permission error
means the Docker engine is not available to the current terminal; start Docker
Desktop, wait for initialization, and open a new terminal. On Linux, follow
Docker's post-install instructions if the account needs permission to use the
daemon.

## 2. Clone and select the development branch

```text
git clone https://github.com/TheMikaus/LumaHome.git
cd LumaHome
git switch lumahome/home-menu-framework
git pull --ff-only
```

Before compiling, `git status --short --branch` should show the expected branch.
Do not build an old checkout or the upstream Luma3DS default branch by mistake.

## 3. Build on Windows

From the repository root in PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\build_docker.ps1
```

The first run builds `lumahome-toolchain:1`. It downloads the pinned devkitARM
base image and builds the two firmware utilities, so it can take several
minutes. Later runs reuse that image and are substantially faster.

The helper mounts the checkout at `/project`, runs `make -j2`, verifies that
`boot.firm` exists, and prints its SHA-256 hash.

## 4. Build on Linux or macOS

From the repository root:

```sh
docker build -t lumahome-toolchain:1 -f Dockerfile .
docker run --rm -v "$PWD:/project" -w /project lumahome-toolchain:1 make -j2
sha256sum boot.firm
```

On Apple Silicon, Docker may use emulation for this image. The build can be
slower, but the output procedure is otherwise the same.

## 5. Rebuild after source changes

Run only the container command when the Dockerfile has not changed:

Windows PowerShell:

```powershell
docker run --rm --volume "${PWD}:/project" --workdir /project lumahome-toolchain:1 make -j2
Get-FileHash -Algorithm SHA256 .\boot.firm
```

Linux or macOS:

```sh
docker run --rm -v "$PWD:/project" -w /project lumahome-toolchain:1 make -j2
sha256sum boot.firm
```

To force a clean compilation, run `make clean` inside the same container, then
run the build again. Do not use Git cleanup commands to clean build output;
they can remove work that has not been committed.

## 6. Locate and deploy the result safely

The build creates `boot.firm` in the repository root. During LumaHome
development, do **not** replace the known-good `/boot.firm` at the root of the
3DS SD card. Instead:

1. Rename the result to a unique version such as `LumaHome010RC48.firm`.
2. Copy it to `/luma/payloads/` on the SD card.
3. Safely eject the card.
4. Hold Start while booting and chainload that versioned payload.
5. Confirm the visible overlay version before testing.

Record the filename, Git commit, and SHA-256 together. This prevents a test
result from being attributed to the wrong firmware build.

## Troubleshooting

- **Cannot connect to Docker / `docker_engine` access denied:** start Docker
  Desktop and wait for it to finish. If Docker was just installed, restart the
  terminal or Windows. Do not run an untrusted build from an elevated shell.
- **Download or certificate failure during `docker build`:** confirm Docker has
  network access, then rerun the same command. Completed layers are cached.
- **`firmtool` or `makerom` missing:** rebuild the image from this repository's
  Dockerfile; do not substitute an arbitrary image with the same tag.
- **Files created with unexpected ownership on Linux:** run the container with
  `--user "$(id -u):$(id -g)"` for incremental builds. The image itself still
  builds as root.
- **Out-of-memory or unexplained parallel failure:** change `make -j2` to
  `make -j1`.
- **Stale result:** delete only generated firmware/object output with
  `docker run --rm -v "$PWD:/project" -w /project lumahome-toolchain:1 make clean`,
  then compile again.
