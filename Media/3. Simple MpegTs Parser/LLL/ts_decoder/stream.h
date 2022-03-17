#ifndef _STREAM_H_
#define _STREAM_H_

#include <string>
#include <fstream>

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned long  uint32_t;
typedef unsigned long long uint64_t;

class Stream
{
public:
	Stream(std::string filepath);
	~Stream();

	uint8_t next_byte();
	uint8_t read_1byte();
	uint16_t read_2bytes();
	uint32_t read_4bytes();
	void skip(int n);
	int pos() const { return m_pos; }

private:
	std::ifstream m_stream;
	int m_pos;
};

#endif // !_STREAM_H_
