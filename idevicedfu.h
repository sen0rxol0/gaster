#ifndef IDEVICEDFU_H
#define IDEVICEDFU_H

char* idevicedfu_info(char *t);
int idevicedfu_find();
void idevicedfu_sendcommand(char* command);
void idevicedfu_sendfile(const char* filepath);
#endif
