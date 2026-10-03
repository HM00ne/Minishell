#ifndef PROC_H
#define PROC_H

int  proc_find_shell(char *out, int size);

int  proc_start(const char *exe_path, char *err, int errlen);

int  proc_read(char *buf, int size);

int  proc_write(const char *data, int len);

void proc_close_input(void);

void proc_interrupt(void);

int  proc_running(int *exit_code);

void proc_stop(void);

#endif
