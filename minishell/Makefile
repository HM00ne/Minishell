# Builds the shell on Linux, macOS, WSL or Cygwin.
# (The graphical terminal is Windows-only: use build.bat for it.)

CC     ?= cc
CFLAGS ?= -Wall -Wextra -pedantic -std=c11 -O2

minishell: Minishell.c
	$(CC) $(CFLAGS) -o $@ Minishell.c

run: minishell
	./minishell

clean:
	rm -f minishell minishell.exe terminal.exe glfw3.dll

.PHONY: run clean
