
#ifndef __STR_H__
#define __STR_H__

#include <errno.h>
#include <list>
#include <string>

#include "sdk_type.h"


class STR
{
public:
    static std::string GetKey(const std::string& source, const std::string& end);
    static std::string GetValue(const std::string& source, const std::string& key, const std::string& end);
    static std::string UInt64ToStr(uint64_t value);
};

#endif
