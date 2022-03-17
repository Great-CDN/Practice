#include <iostream>
#include <string>

#include "stream.h"
#include "ts.h"


int main(int argc, const char* argv[])
{
	if (argc != 2) {
		std::cerr << "Usage: " << std::endl;
		std::cerr << "\t" << argv[0] << " <tsfile>" << std::endl;
		return 1;
	}

	std::string tsfile = argv[1];
	Stream s(tsfile);
	TS::decode(s);

	return 0;
}