#ifndef P2S_CTL_H
#define P2S_CTL_H

// Control protocol server on the shell port + 3, see CTL_PROTOCOL.md.

void ctl_init();

int ctl_open(int port);

void ctl_close();

void ctl_start();

void ctl_exit();

// 1 while a control command runs legacy shell code on the calling thread
int ctl_capturing();

// Called by psp2shell_print_color: returns 1 if the text was consumed as output of a control
// command running on the calling thread.
int ctl_capture(int color, const char *text);

#endif // P2S_CTL_H
