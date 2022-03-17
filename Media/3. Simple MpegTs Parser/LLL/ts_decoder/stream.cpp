#include <fstream>

#include "stream.h"


Stream::Stream(std::string filename) :m_pos(0)
{
	m_stream.open(filename.c_str());
}

Stream::~Stream()
{
	m_stream.close();
}

uint8_t Stream::next_byte()
{
	return m_stream.peek();
}

uint8_t Stream::read_1byte()
{
	m_pos += 1;

	return m_stream.get();
}

uint16_t Stream::read_2bytes()
{
	m_pos += 2;

	uint8_t bytes[2];
	m_stream.read((char*)bytes, 2);
	return (bytes[1] << 0) | (bytes[0] << 8);
}

uint32_t Stream::read_4bytes()
{
	m_pos += 4;

	uint8_t bytes[4];
	m_stream.read((char*)bytes, 4);
	return (bytes[3] << 0) | (bytes[2] << 8) | (bytes[1] << 16) | (bytes[0] << 24);
}

void Stream::skip(int n)
{
	m_pos += n;

	m_stream.ignore(n);
}
