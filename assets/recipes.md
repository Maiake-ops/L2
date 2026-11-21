# Writing L2PM recipes

An L2PM script is a `.recipe` file containing `key=value` lines. L2PM reads
the metadata, prepares the source, runs shell commands, and packs the files
staged in `$DESTDIR` into a `.l2p` package. Recipes run on the build host;
installing the resulting package does not require its compiler or source.

## First recipe: package a shell script

Save this as `hello.recipe`:

```ini
name=hello
version=1.0.0
epoch=315532800
install=mkdir -p "$DESTDIR/usr/bin" && printf '%s\n' '#!/bin/sh' 'echo Hello from L2PM' > "$DESTDIR/usr/bin/hello" && chmod 0755 "$DESTDIR/usr/bin/hello"
```

This recipe needs no source download or compilation. Its `install=` command
creates an executable inside the package's staging directory.

From the repository root, build L2PM and try the recipe using a temporary
package root (no root privileges needed):

```sh
make -C l2pm
export L2PM_ROOT="$(mktemp -d)"
./l2pm/l2pm build ./hello.recipe --check -o "$L2PM_ROOT/hello.l2p"
./l2pm/l2pm install "$L2PM_ROOT/hello.l2p"
"$L2PM_ROOT/l2/var/profiles/default/usr/bin/hello"
```

The last command prints `Hello from L2PM`. `--check` builds twice and compares
the archives' SHA-256 hashes. Keep `L2PM_ROOT` set for the rest of this guide
to keep package state in that temporary directory. With it unset, L2PM uses
`/l2`. The examples below assume `l2pm` is available on your `PATH`; when
working from this checkout, use `./l2pm/l2pm` instead.

## Syntax rules

- Put one assignment on each physical line. Spaces around the key and value
  are trimmed; the first `=` separates them.
- Blank lines and lines starting with `#` after trimming are ignored.
  There is no general inline-comment syntax: `version=1.0 # release` includes
  the comment in the value and fails validation.
- Metadata is literal text. Write `name=hello`, not `name="hello"`.
  Metadata values do not expand `$VARIABLES` or shell substitutions.
- `compile=`, `install=`, and `build=` values are shell commands. Use shell
  quoting inside them, especially around `"$DESTDIR"` and `"$SRC"`.
- There are no multiline values, backslash continuations, or recipe-level
  functions. Keep commands on one line, or invoke a script included in the
  source, such as `compile=sh ./build.sh`.
- Unknown keys and lines without `=` are ignored. Repeated keys use the last
  value. Check spelling carefully: an ignored command key can leave an empty
  package.

## Recipe fields

| Field | Meaning |
| --- | --- |
| `name` | Required package name. Letters, digits, `.`, `_`, `+`, and `-` are accepted. |
| `version` | Required version, with the same allowed characters as `name`. |
| `source` | Local file/directory, `file://` path, or HTTP(S) download. Relative local paths resolve from the recipe's directory. Optional for generated files. |
| `sha256` | Expected source-file hash. Omit for directories. Empty or `-` skips verification; use a real hash for downloaded archives. |
| `repo` | Git source repository, used instead of `source`. Requires Git on the build host. |
| `ref` | Required with `repo`. Prefer a full commit hash; branches and tags are accepted with a reproducibility warning. |
| `source_dir` | Working subdirectory within the prepared source. `srcdir` is an alias. |
| `compile` | Shell command to compile the source. Requires `install`. |
| `install` | Shell command to put package files under `$DESTDIR`. Can be used alone. |
| `build` | Legacy single command combining compilation and staging. |
| `deps` | Dependency recipe names (whitespace or comma separated). `l2pm add` automatically builds and installs missing dependencies; `l2pm remove` refuses if installed packages depend on the target unless `--force` is given. |
| `epoch` | Unix timestamp for package metadata. Overrides `SOURCE_DATE_EPOCH`; the fallback is `315532800` (1980-01-01 UTC). |

Do not combine `source` with `repo`, use `ref` without `repo`, or combine
`build` with either `compile` or `install`. `sha256` is ignored with `repo`.
Keep names and versions below 128 bytes and each command below 16,384 bytes:
the parser rejects oversized values before executing any build commands and
reports the recipe path, line number, field, and byte limit.

## How execution works

1. **Select source:** use the local/download source or clone and check out the
   Git repository at `ref`.
2. **Prepare source:** copy a local tree/file, extract a source archive, or
   export the Git worktree without `.git`. Git source timestamps are normalized
   to `epoch`. Without a source, start with an empty source directory.
3. **Compile:** run `compile`, if supplied.
4. **Stage:** run `install`, then package everything under `$DESTDIR`, including
   generated `.l2pm/info` metadata. With `build`, one command handles both
   compilation and staging.

Each command runs in a separate `/bin/sh -e -u -c` process, starting in the
source root. Variables exported or directories changed during `compile` do
not carry into `install`; files created on disk do. Use POSIX shell syntax.
Normal shell `-e` exceptions still apply, so avoid masking required failures
with `|| true`.

If `source_dir` is absent and the prepared source contains exactly one entry
that is a directory, L2PM enters that directory automatically. Otherwise it
uses the prepared source directory itself. Set `source_dir=.` to explicitly
stay at that root.

| Shell variable | Value |
| --- | --- |
| `DESTDIR` | Empty staging directory whose contents become the package. |
| `SRC` | Selected source root; also the initial working directory. |
| `PREFIX` | `/usr`. |
| `CC` | Inherited compiler setting, or `cc`. |
| `JOBS` | Inherited parallelism setting, or `8`. |
| `SOURCE_DATE_EPOCH` | Selected recipe timestamp. |
| `TZ` | `UTC`. |
| `LC_ALL`, `LANG` | `C`. |

L2PM sets the file creation mask to `022`. The build host must already provide
the tools used by a recipe, such as `make`, a compiler, or archive decompressors.
HTTP(S) fetching uses `curl`, with `wget` as a fallback. Source archives are
extracted with the host's `tar`; `.l2p` installation uses L2PM's built-in reader.

Recipes execute with your user permissions, without a sandbox. `L2PM_ROOT`
relocates package state; it does not confine shell commands. Always stage into
`$DESTDIR` instead of writing directly to `/usr`.

## Compile a local C program

Place `hello.c` beside `hello.recipe`:

```c
#include <stdio.h>
int main(void) { puts("Hello from C"); return 0; }
```

Use this recipe:

```ini
name=hello-c
version=1.0.0
source=hello.c
compile="$CC" -Os -static -o hello hello.c
install=mkdir -p "$DESTDIR$PREFIX/bin" && install -m 0755 hello "$DESTDIR$PREFIX/bin/hello-c"
```

Run `l2pm build ./hello.recipe --check`. This example requires a compiler with
static libraries; for L2, select an appropriate static toolchain through `CC`.
For example, from the repository root after the musl wrapper has been built:

```sh
CC="$PWD/build/bin/musl-gcc" l2pm build ./hello.recipe --check
```

## Build an archive or Git project

For a project supporting `configure` and `make install`, adapt this template.
Replace the URL, hash, name, and version with the actual release values:

```ini
name=example
version=1.0.0
source=https://example.org/example-1.0.0.tar.gz
sha256=REPLACE_WITH_THE_ARCHIVE_SHA256
compile=./configure --prefix="$PREFIX" && make -j"$JOBS"
install=make DESTDIR="$DESTDIR" install
```

Obtain a local archive's hash with `l2pm hash ./example-1.0.0.tar.gz` and verify
it against the publisher's trusted release information. Supported source
archive suffixes are `.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.tar.bz2`,
`.tbz2`, and `.tar.zst`.

To use Git instead, replace the `source` and `sha256` lines with:

```ini
repo=https://example.org/example.git
ref=REPLACE_WITH_THE_FULL_COMMIT_HASH
```

The recipe's `repo` chooses the **package source**. The environment variable
`L2PM_RECIPE_REPO` separately chooses a repository from which L2PM looks up
**recipe files**. Newly fetched remote recipes require confirmation or an
explicit `--trust`; accepted recipe content hashes are remembered. Local
recipe paths run without that confirmation.

## Build, install, and troubleshoot

```sh
l2pm build ./hello.recipe --check -o ./hello.l2p
l2pm install ./hello.l2p
# Or build and install in one command:
l2pm add ./hello.recipe
```

`build` creates an archive only; `install=` inside a recipe stages its files.
The CLI `install` command puts an archive into the content-addressed store
and activates profile symlinks. Without `-o`, builds write to
`$L2PM_ROOT/l2/cache/<name>-<version>.l2p` (or `/l2/cache/...` without a root).
Packages are plain, uncompressed ustar archives with sorted entries, fixed
timestamps, and UID/GID zero. Reproducible archive metadata alone does not
guarantee reproducible compiler output; use `--check` to test the result.

Use `L2PM_RECIPES=/path/to/recipes l2pm build hello` to look up a recipe by
name. Explicit recipe paths are checked first, then `L2PM_RECIPES`, the root's
`l2/recipes`, `./<name>.recipe`, and finally the configured remote repository.

Set `L2PM_KEEP=1` to retain build directories for inspection under
`$L2PM_ROOT/l2/build/`. A successful command that stages nothing produces only
package metadata: check that the install command writes under `$DESTDIR`.
When `install` cannot find an artifact made during `compile`, check the
working directory and remember that the two commands use separate shells.

For existing recipes, see [l2pm/recipes](l2pm/recipes), especially the small
[l2fetch recipe](l2pm/recipes/l2fetch.recipe). The implementation lives in
[l2pm/l2pm.c](l2pm/l2pm.c), principally `parse_recipe`, `build_once`, and
`run_shell`.
