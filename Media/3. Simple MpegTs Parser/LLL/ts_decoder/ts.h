#ifndef _TS_H_
#define _TS_H_

#include "stream.h"

#define TS_SYNC_BYTE 0x47
#define TS_STUFFING_BYTE 0xff
#define TS_PAT_PID 0x00
#define TS_PACKET_SIZE 188

class TS
{
public:
	TS();
	~TS();
	static void decode(Stream& s);

private:

};

#endif // !_TS_H_
