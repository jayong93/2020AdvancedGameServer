#pragma once
#include <WS2tcpip.h>
#include <MSWSock.h>

#ifdef NETWORK_IMPL
#pragma comment(lib, "Ws2_32.lib")
#define EXTERN
#else
#define EXTERN extern
#endif
EXTERN RIO_EXTENSION_FUNCTION_TABLE rio_ftable;
EXTERN PCHAR rio_buffer;
EXTERN RIO_BUFFERID rio_buf_id;

#undef EXTERN

void error_display(const char* msg, int err_no);
