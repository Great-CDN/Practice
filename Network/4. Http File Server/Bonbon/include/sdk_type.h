
#ifndef __SDK_TYPE_H__
#define __SDK_TYPE_H__

#include <assert.h>
#include <stdint.h>

#define TRUE  1
#define FALSE 0

typedef uint8_t Bool;
typedef int     FD;
typedef int     ErrCode;

#define EC_OK  0
#define EC_ERR 1
#define EC_EOF 11
#define Assert assert

#endif
