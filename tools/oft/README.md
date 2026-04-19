# OpenFastTrace (project-local install)

This directory holds a project-local install of
[OpenFastTrace](https://github.com/itsallcode/openfasttrace) (OFT), the
spec-traceability tool described in
[`docs/spec-system.md`](../../docs/spec-system.md).

## Requirements

- Java 17 or later on `PATH`. The wrappers will fall back to a
  `C:\Program Files\Microsoft\jdk-*-hotspot` install if `java` is not on
  `PATH` (useful right after a fresh winget install before the shell is
  restarted).

## Setup

The JAR itself is not committed. To download the project-pinned version
and verify its SHA256:

```bash
tools/oft/install.sh
```

Re-running `install.sh` is idempotent.

## Usage

From bash:

```bash
tools/oft/oft.sh trace specs/ src/ test/
tools/oft/oft.sh trace -o html -f trace.html specs/ src/ test/
tools/oft/oft.sh help
```

From cmd.exe:

```
tools\oft\oft.cmd trace specs\ src\ test\
```

## Pinned version

OFT is pinned to **4.2.2**. The version string lives in three files
(`install.sh`, `oft.sh`, `oft.cmd`). To upgrade, edit all three; the SHA256
is fetched from the upstream release alongside the JAR, so there is no
hand-maintained checksum.
