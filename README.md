# Minishell

A small Unix shell written in C, plus its own graphical terminal window for Windows.

The shell reads a command, splits it into words, and runs programs with `fork`, `execvp` and `waitpid`, the same system calls that bash uses. It's a compact project for learning how processes, pipes, file descriptors and signals work.

## Features

**Shell (`Minishell.c`)**

| Feature | Example |
|---|---|
| Run any program on `PATH` | `ls -l /tmp` |
| Pipelines (any length) | `cat file \| sort \| uniq -c` |
| Redirection | `sort < in.txt > out.txt`, `echo hi >> log.txt` |
| Command lists | `cd /tmp; ls; echo done` |
| Background jobs | `sleep 5 & echo "not waiting"` |
| Quotes and escapes | `echo 'literal $HOME' "expanded $HOME" a\ b` |
| Variables | `$HOME`, `$PATH`, `$?` (last exit status) |
| Built-ins | `cd [dir \| -]`, `pwd`, `export NAME=value`, `unset NAME`, `exit [code]`, `help` |
| Signals | Ctrl+C stops the running command, not the shell. Ctrl+D exits. |

**Terminal window (`terminal/`, Windows)**

- Starts the shell as a child process and talks to it through pipes
- Shows ANSI colors (colored prompt, `ls --color`, and so on)
- Line editing, command history (↑/↓), paste, scrollback (5000 lines)
- Ctrl+C sends `SIGINT` to the running command
- Resizable, zoomable, and scales correctly on HiDPI displays

## Getting started

### Windows (shell + terminal window)

Requirements:
- [Cygwin](https://www.cygwin.com/) with `gcc-core`, installed in `C:\cygwin64`. It provides `fork`/`exec` and the commands (`ls`, `cat`, …).
- [MSYS2](https://www.msys2.org/) with raylib, installed in `C:\msys64`:
  ```
  pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-raylib
  ```

Build and run:
```bat
build.bat
terminal.exe
```

`build.bat` creates `minishell.exe`, `terminal.exe` and a copy of `glfw3.dll`. Keep all three in the same folder.

You can also run the shell directly in a Cygwin terminal: `./minishell.exe`

### Linux / macOS / WSL (shell only)

```sh
make
./minishell
```

## Usage

```
minishell:/tmp$ echo "Hello" | tr a-z A-Z
HELLO
minishell:/tmp$ export NAME=world; echo "hi $NAME"
hi world
minishell:/tmp$ false; echo "status: $?"
status: 1
minishell:/tmp$ ls > files.txt; wc -l < files.txt
12
```

### Terminal shortcuts

| Key | Action |
|---|---|
| Enter | Run the line |
| ↑ / ↓ | Command history |
| ← / →, Home / End | Move the cursor (Ctrl+A / Ctrl+E also work) |
| Ctrl+C | Interrupt the running command |
| Ctrl+D | End of input (exits the shell when the line is empty) |
| Ctrl+L | Clear the screen |
| Ctrl+U | Clear the input line |
| Ctrl+V | Paste |
| Ctrl + / − / 0 | Zoom in / out / reset |
| Mouse wheel, PageUp / PageDown, Shift+↑/↓ | Scroll |

## How it works

```
   terminal.exe                         minishell.exe                    child processes
 ┌──────────────┐   your input (pipe)   ┌─────────────────┐   fork+exec   ┌─────────────┐
 │ draw text    │ ────────────────────► │ read line       │ ────────────► │ ls, grep,   │
 │ edit line    │                       │ tokenize        │               │ sort, ...   │
 │ ANSI colors  │ ◄──────────────────── │ parse           │ ◄──────────── │             │
 └──────────────┘   output (pipe)       │ execute         │   waitpid     └─────────────┘
                                        └─────────────────┘
```

For each command, the shell goes through four steps:

1. **Read** a line (`fgets`).
2. **Tokenize** (`tokenize()`): split it into words and operators (`|`, `<`, `>`, `>>`). This step also handles quotes and expands `$VAR`. It stops at `;` or `&`, so each command is expanded only after the previous one has finished. That's why `false; echo $?` prints `1`.
3. **Parse** (`parse()`): group the tokens into the commands of a pipeline, each with its own `argv`, input file and output file.
4. **Execute** (`execute()`): create a `pipe()` between neighboring commands, then `fork()` each one. In the child, `dup2()` connects stdin and stdout to the pipe or file, then `execvp()` runs the program. The shell then calls `waitpid()` for every child.

Built-ins such as `cd` run **inside the shell process**. A child changing its own directory would not affect the shell.

The terminal window connects to the shell through pipes, not a real TTY, so it starts the shell with `-i` (interactive mode). When you press Ctrl+C, the window signals the shell, and the shell forwards `SIGINT` to the commands running in the foreground.

## Project structure

```
Minishell.c          the shell (portable POSIX C)
terminal/
  terminal.c         terminal window: drawing, input, ANSI parser (raylib)
  proc_win.c         starting the shell and its pipes (Win32 API)
  proc.h             interface between the two
build.bat            Windows build (shell + terminal)
Makefile             Linux/macOS build (shell)
docs/screenshot.png
```

## Limitations

- No `&&` / `||`, wildcards (`*.c`), `2>` redirection, heredocs (`<<`) or job control (`fg`, `bg`, `jobs`)
- The terminal works line by line, so full-screen programs like `vim`, `nano` and `less` don't display correctly
- No mouse text selection in the terminal yet

These would make good next steps if you want to extend the project.
