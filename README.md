# ALT-Shell

A small Linux command shell written in C++17. The distinguishing feature is **context capsules**: save a working directory plus explicitly selected environment variables, then switch between project contexts without accidentally persisting every secret in your environment.

## Features

- External programs via `fork`/`execvp` (no `system()` or implicit `/bin/sh` execution).
- Pipelines (`|`), input/output redirection (`<`, `>`, `>>`, `2>`), command separators (`;`) and background launch (`&`).
- Quotes, backslash escapes, `$NAME`, `${NAME}`, `$?`, `$$` and `~` expansion.
- Built-ins: `cd`, `pwd`, `back`, `croot`, `history`, `recall`, `env`, `export`, `capsule`, `clear`, `version`, `help`, `exit`.
- Persistent command history (last 2,000 lines).
- Color prompt with current directory and last exit status.
- Context capsule data is stored under `~/.config/alt-shell/` (or `XDG_CONFIG_HOME`) with restrictive directory/file permissions. Only variables named in `capsule save` are stored.

## Build

```sh
make
make test
./alt
```

Install system-wide (optional):

```sh
sudo make install
```

## Context capsules

```sh
cd ~/src/my-project
export EDITOR=nvim
capsule save my-project EDITOR JAVA_HOME

cd /tmp
export EDITOR=vi
capsule diff my-project     # shows how the current context differs
capsule load my-project     # restores cwd and the listed variables
capsule list
capsule show my-project
```

A capsule stores only the directory and variable names explicitly passed to `save`. This is deliberate: automatically serializing the entire environment might save API keys, access tokens, or other secrets. Variables that were unset at save time are restored as unset.

Other examples:

```sh
printf 'hello\n' | grep hello
ls -la > listing.txt
cat < listing.txt
sleep 5 &
recall capsule
cd -
back
croot   # jump to the nearest parent directory containing .git
```

## Current limitations

This is an early, intentionally small shell—not a drop-in Bash replacement. It does not yet implement shell scripting syntax, `&&`/`||`, command substitution, glob expansion, here-documents, full job control, or shell-compatible expansion rules in every edge case. Background jobs are started without an interactive job-control mechanism.

## License

Mozilla Public License 2.0 (MPL-2.0). See [LICENSE](LICENSE).
