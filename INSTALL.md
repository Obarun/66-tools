# Build Instructions

## Requirements

To build and install the 66-tools project, you need:

- A POSIX-compliant C development environment (conforming to POSIX.1-2008, available at Open Group).

- `Meson` version `1.1.0` or later: [mesonbuild.com](https://mesonbuild.com).

- `Ninja` (typically installed with Meson).

- `execline` version `2.9.6.1` or later: [skarnet.org/software/execline](https://skarnet.org/software/execline).

- `oblibs` version `0.3.4.0` or later: [git.obarun.org/Obarun/oblibs](https://git.obarun.org/Obarun/oblibs).

- `66` version `0.8.0.0` or later (required only when `enable-dbus` is not `disabled`): [git.obarun.org/Obarun/66](https://git.obarun.org/Obarun/66).

- `lowdown` version `0.6.4` or later (optional, for generating man pages and HTML documentation): [kristaps.bsd.lv/lowdown](https://kristaps.bsd.lv/lowdown).

- Linux API headers version 5.8 or later (required for Linux systems): [gnu.org/software/libc](https://gnu.org/software/libc).

The 66-tools no longer use `skalibs` directly, but it remains a transitive
dependency of `oblibs` and `execline` and must therefore be present on the system.

The software is designed to install on any operating system implementing `POSIX.1-2008`.

## Standard Usage

For most users, the following commands will configure, build, and install the 66-tools project with default settings:

```bash
meson setup build
meson compile -C build
meson install -C build
```

This installs:

- Executables to `/usr/bin` or `/usr/libexec` (depending on the executable).
- Header files to `/usr/include/66-tools`.
- Documentation to `/usr/share/doc/66-tools` (HTML) and `/usr/share/man` (man pages).

To reduce binary size, you can strip symbols before installation:

```bash
meson compile -C build --strip
meson install -C build
```

Documentation (man pages and HTML) is generated and installed only if **lowdown is installed** and the `with-doc` option is enabled (default: `false`).

## Customization

You can customize the build using Meson options. To see all available options, run:

```bash
meson configure build
```

Example customization:

```bash
meson setup build -D prefix=/usr/local -D enable-shared=false -D enable-static=true -D enable-static-deps=true -D test=true
meson compile -C build
meson install -C build
```

## Key options include:

- `ns-rule-dir`: Set the installation directory for 66-ns rules (default: `/usr/share/66/script/ns`).
- `enable-dbus`: Enable 66-dbus-launch with support for basu or elogind (choices: `disabled`, `basu`, `elogind`; default: `disabled`). Any value other than `disabled` requires `lib66`.
- `dbus-system-service-dir`: Set the directory for DBus system service files (default: `/usr/share/dbus-1/system-services`).
- `dbus-session-service-dir`: Set the directory for DBus session service files (default: `/usr/share/dbus-1/services`).
- `dbus-system-name`: Specify the name of the DBus system socket (default: `system_bus_socket`).
- `dbus-session-name`: Specify the name of the DBus session socket (default: `dbus`).
- `enable-shared`: Build shared libraries for dynamic linking (default: `true`).
- `enable-static`: Build static libraries for static linking (default: `false`).
- `enable-static-deps`: Prefer static linking for dependencies (e.g., `oblibs`, `execline`) to reduce runtime dependencies; requires `-D enable-static=true` (default: `false`).
- `enable-static-executable`: Build fully static executables, including a static `libc`, for maximum portability; requires a static `libc` (e.g., `libc.a`) on the system (default: `false`).
- `enable-all-pic`: Compile static libraries with position-independent code (`PIC`) for use in shared libraries or `PIE` executables (default: `false`).
- `enable-pie`: Build executables as position-independent (`PIE`) for enhanced security via Address Space Layout Randomization (`ASLR`) (default: `false`).
- `with-doc`: Build and install man pages and HTML documentation (default: `false`).
- `doc-only`: Build only the documentation, skipping the C sources and their dependencies; requires `with-doc=true` (default: `false`).
- `test`: Build and run tests (default: `false`).

## Option Combinations

- You can enable both `enable-shared` and `enable-static` to build **both** shared and static libraries.
- `enable-static-deps` requires `enable-static=true`.
- `enable-static-executable` conflicts with `enable-shared` and requires a static `libc`.
- `enable-static-deps` conflicts with `enable-shared` due to incompatible linking models.
- `enable-pie` is compatible with most options but may not work with `enable-static-executable` on some systems due to toolchain limitations.
- `enable-all-pic` applies only to static libraries and is compatible with all options.
- `doc-only` requires `with-doc=true`.

## Environment Variables

Meson supports a few environment variables for build customization, but passing options directly to meson setup is preferred for clarity:

- `CC`: Overrides the compiler (e.g., `CC=clang meson setup build`). When cross-compiling, the `--cross-file` option may prefix the compiler with the target triplet.

- `CFLAGS`, `CPPFLAGS`, `LDFLAGS`: Appended to Meson’s default flags. To override defaults, use Meson options or build variables instead.

## Build Variables

You can pass variables to meson compile or meson install for fine-grained control:

- `CC`, `CFLAGS`, `CPPFLAGS`, `LDFLAGS`, `LDLIBS`: Override compiler, flags, or libraries.
- `AR`, `RANLIB`, `STRIP`, `INSTALL`: Customize archiver, ranlib, strip, or install tools.
- `DESTDIR`: Specify a staging directory for installation.

Example:

```bash
CFLAGS="-O3 -march=native" meson compile -C build
DESTDIR=/tmp/staging meson install -C build
```

# Static Binaries

By default, executables are linked dynamically with `libc` and other dependencies, even if `enable-static` is used. To build fully static executables (including a static `libc`):

- Use `enable-static-executable`. This requires a static `libc` (e.g., `libc.a`) on the system, such as `libc6-dev` (Debian/Ubuntu), `glibc-static` (Fedora), or `musl-dev` (Alpine Linux).

- Note: GNU `libc` produces larger static binaries compared to alternatives like `musl`. For smaller, portable binaries, consider using `musl`.

To reduce runtime dependencies without fully static executables, use `enable-static-deps` with `enable-static=true` to link dependencies (e.g., `oblibs`, `execline`) statically.

## Cross-Compilation

To cross-compile:

- Create a Meson cross file (e.g., `cross-file.ini`) specifying the target triplet (e.g., `arm-linux-gnueabihf`) and toolchain paths.
- Ensure the cross-toolchain binaries (e.g., `arm-linux-gnueabihf-gcc`) are in your `PATH`.
- Ensure the dependencies (`execline`, `oblibs`, and their own transitive dependencies) are built for the target platform.
- Customize include and library paths with `with-include-dir`, `with-staticlib-dir`, and `with-dynamiclib-dir` if needed.

Example:

```bash
meson setup build --cross-file=cross-file.ini -D with-include-dir=/path/to/include
meson compile -C build
meson install -C build
```

## Notes

- If `enable-static-executable` is enabled, the build will fail with a clear error if a static `libc` is not found. Ensure the appropriate development package is installed (e.g., `libc6-dev`, `musl-dev`).

- If `enable-static-deps` is enabled without `enable-static`, the build will fail to ensure static libraries are available.

- For security-sensitive systems, consider enabling `enable-pie` to benefit from `ASLR`.

- Documentation requires `lowdown`. If unavailable or `with-doc=false`, no documentation will be installed.
</content>
