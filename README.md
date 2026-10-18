# ALT-Shell

ALT-Shell is an experimental Linux command shell written in C++17. It has a small interactive line editor and runs external commands in a configurable Bubblewrap sandbox by default.

## Features

- **Arrow-key editing**: Up/Down command history; Left/Right cursor movement; Home/End; Delete/Backspace; Ctrl+A/E/U/K/W/L.
- External programs through `fork`/`execvp`, with sandboxing through `bwrap` enabled by default.
- **Configurable sandbox**: private user/PID/IPC/UTS namespaces, network isolated by default, selected read-only system directories, a selected workspace, limited environment variables, optional command allowlist, and extra read-only/writable mounts.
- Pipelines (`|`), input/output redirection (`<`, `>`, `>>`, `2>`), command separators (`;`) and background launch (`&`).
- Quotes, escapes, `$NAME`, `${NAME}`, `$?`, `$$`, and `~` expansion.
- Built-ins: `cd`, `pwd`, `back`, `croot`, `history`, `recall`, `env`, `export`, `capsule`, `clear`, `version`, `help`, `exit`.
- Persistent command history (last 2,000 lines).
- **Context capsules**: save a directory plus explicitly selected environment variables; inspect diffs and restore later.
- Missing `bwrap` does **not** cause an unsafe fallback: external commands remain blocked while sandboxing is enabled.

## Build

```sh
make
make test
./alt
```

On EndeavourOS/Arch Linux, install the runtime dependency with:

```sh
sudo pacman -S bubblewrap
```

## Configuration

On first start, ALT-Shell creates `~/.config/alt-shell/config` (or `$XDG_CONFIG_HOME/alt-shell/config`). A commented example is available in [`alt-shell.conf.example`](alt-shell.conf.example).

```ini
container.enabled=true
container.network=false
container.workspace_writable=true
container.readonly_paths=/usr,/etc,/bin,/sbin,/lib,/lib64
container.extra_readonly=
container.extra_writable=
container.allowed_commands=
container.pass_env=TERM,LANG,LC_ALL,USER,LOGNAME,EDITOR
```

| Key | Purpose |
|---|---|
| `container.enabled` | Run external commands through Bubblewrap. `true` by default. |
| `container.network` | Allow host networking inside the sandbox. `false` by default. |
| `container.workspace_writable` | Mount the startup working directory writable (`true`) or read-only (`false`). |
| `container.readonly_paths` | Comma-separated system directories visible read-only inside the sandbox. |
| `container.extra_readonly` | Extra host directories visible as `/mnt/alt-ro-0`, `/mnt/alt-ro-1`, etc. |
| `container.extra_writable` | Extra explicitly granted writable host directories as `/mnt/alt-rw-0`, `/mnt/alt-rw-1`, etc. |
| `container.allowed_commands` | Optional comma-separated executable-name allowlist. Empty means any command may run *inside* the sandbox. |
| `container.pass_env` | Host environment variables permitted into sandboxed commands. `HOME` and `PATH` are configured internally. |

**Important:** launch ALT-Shell from the project/work directory you intend to expose, not from your whole home directory. That directory is mounted at `/workspace`; extra configured mounts appear under `/mnt/alt-ro-N` and `/mnt/alt-rw-N`. Shell built-ins operate in the shell process, while external programs are sandboxed. Install Bubblewrap and ensure your kernel/user-namespace policy permits it; if Bubblewrap cannot start, ALT-Shell does not run the command outside the sandbox.

The sandbox is defense-in-depth for ordinary development commands, not a hardened boundary for deliberately hostile code. It does not currently configure CPU/memory limits or a custom seccomp policy. Redirections in sandbox mode are restricted to the current workspace; output redirections are refused when the workspace is read-only.

## Context capsules

```sh
cd ~/src/my-project
export EDITOR=nvim
capsule save my-project EDITOR JAVA_HOME

cd /tmp
capsule diff my-project
capsule load my-project
capsule list
capsule show my-project
```

Only variables explicitly named on `capsule save` are persisted, to avoid accidentally saving API keys and tokens. Variables that were unset are restored as unset.

## Other examples

```sh
printf 'hello\n' | grep hello
ls -la > listing.txt
cat < listing.txt
sleep 5 &
recall capsule
cd -
back
croot
```

## Current limitations

This is an early shell, not a Bash replacement. It does not yet implement full job control, `&&`/`||`, command substitution, glob expansion, here-documents, or shell-compatible expansion in every edge case.

## License

Mozilla Public License 2.0 (MPL-2.0). See [LICENSE](LICENSE).
