
#include <cstdio>
#include <iostream>
#include <string>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <map>

#define RB8(x)  ((const uint8_t *)(x))[0]
#define RB16(x) ((uint16_t)(((const uint8_t *)(x))[0] << 8) | ((const uint8_t *)(x))[1])

#define EC_OK 0
#define EC_INVAL EINVAL

#define INFO(x)  std::cout << GetTime() << "\tINFO\t" << x << " [" << __FILE__ << ":" << __LINE__ << "]" << std::endl
#define ERROR(x) std::cout << GetTime() << "\tERROR\t" << x << " [" << __FILE__ << ":" << __LINE__ << "]" << std::endl

std::string GetTime()
{
    time_t    t = time(NULL);
    struct tm tm_now;
    localtime_r(&t, &tm_now);
    char str_time[20] = { 0 };
    strftime(str_time, 20, "%Y-%m-%d %H:%M:%S", &tm_now);
    return std::string(str_time, 19);
}

std::string Int64ToStr(int64_t value)
{
    char buf[21] = { 0 };
    snprintf(buf, sizeof(buf), "%lld", value);
    return std::string(buf);
}

int main(int argc, char** argv)
{
    do
    {
        int err = EC_OK;
        if (argc != 2)
        {
            ERROR("usage: test <mpegts file>");
            break;
        }

        char* file_name = argv[1];
        int fd = open(file_name, O_RDWR | O_LARGEFILE);
        if (fd == -1)
        {
            ERROR("failed to open mpegts file, err: " << errno);
            break;
        }
        int64_t capacity = 10 * 2 * 1024 * (188 * 5);
        uint8_t* buf = new uint8_t[capacity];
        int64_t N = 0;
        int64_t parsed = 0;
        int pmt_pid = 0, v_pid = 0, a_pid = 0;
        std::map<int, uint8_t> stream_types;
        do
        {
            N = read(fd, buf, capacity);
            for (int i = 0; i < N; i += 188)
            {
                if (*(buf + i) != 0x47)
                {
                    err = EC_INVAL;
                    ERROR("[file:" << file_name << "@" << (parsed + i) / 188 << "] sync byte is not 0x47, byte: " << (int)*(buf + i));
                    break;
                }
                std::string type;
                uint8_t payload_unit_start_indicator = *(buf + i + 1) & 0x40;
                int pid = (RB16(buf + i + 1)) & 0x1FFF;
                if (!pid)
                {
                    type = "PAT";
                    pmt_pid = (RB16(buf + i + 15)) & 0x1FFF;
                }
                else if (pid == pmt_pid)
                {
                    type = "PMT";
                    int section_length = RB16(buf + i + 6) & 0x0FFF;
                    int program_info_length = RB16(buf + i + 15) & 0x0FFF;
                    int n = 0;
                    while (section_length - 13 - n)
                    {
                        uint8_t stream_type = RB8(buf + i + 17 + program_info_length + n);
                        int es_pid = RB16(buf + i + 18 + program_info_length + n) & 0x1FFF;
                        stream_types[es_pid] = stream_type;
                        int es_info_length = RB16(buf + i + 20 + program_info_length + n) & 0x0FFF;
                        n += 5 + es_info_length;
                    }
                }
                else if (stream_types.count(pid))
                {
                    type = "PES";
                    if (stream_types[pid] == 0x0F)
                    {
                        type += "(AAC in ADTS)";
                    }
                    else if (stream_types[pid] == 0x11)
                    {
                        type += "(AAC in LATM)";
                    }
                    else if (stream_types[pid] == 0x1B)
                    {
                        type += "(AVC)";
                    }
                    else if (stream_types[pid] == 0x24)
                    {
                        type += "(HEVC)";
                    }
                    else
                    {
                        type += "(unknown)";
                    }
                }
                else
                {
                    type = "unknown";
                }
                if (type != "PES" || payload_unit_start_indicator)
                {
                    INFO("[file:" << file_name << "@" << (parsed + i) << "] ts packet, pid: " << pid << ", type: " << type);
                }
            }
            parsed += N;
        } while (!err && N);
        INFO("[file:" << file_name << "] parse file complete, parsed: " << parsed << ", err: " << err);
    } while (0);
    return 0;
}
