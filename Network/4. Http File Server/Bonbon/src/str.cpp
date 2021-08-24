
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "str.h"

std::string STR::GetKey(const std::string &_source, const std::string &end)
{
    Assert(!end.empty());

    const char *head = _source.c_str();
    const char *tail = head + _source.length();
    do
    {
        if (*head && !end.empty())
        {
            const char *e = strstr(head, end.c_str());
            if (e)
            {
                tail = e;
            }
        }
    } while (0);

    // return first match
    return std::string(head, tail - head);
}

std::string STR::GetValue(const std::string &_source, const std::string &key, const std::string &end)
{
    const char *head = _source.c_str();
    const char *tail = head + _source.length();
    do
    {
        if (key.empty())
        {
            head = tail;
            break;
        }
        head = strstr(head, key.c_str());
        if (!head)
        {
            head = tail;
            break;
        }
        head += key.length();

        if (*head && !end.empty())
        {
            const char *t = strstr(head, end.c_str());
            if (t)
            {
                tail = t;
            }
        }
    } while (0);

    // return first match
    return std::string(head, tail - head);
}

std::string STR::UInt64ToStr(uint64_t value)
{
    char buf[21] = {0};
    //%"PRIu64"
    snprintf(buf, sizeof(buf), "%llu", value);
    return std::string(buf);
}
