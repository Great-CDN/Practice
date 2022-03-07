#include <map>
#include <string>

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef unsigned char      uint8_t;
typedef unsigned short int uint16_t;

enum Codec
{
    AVC = 1,
    HEVC,
    AAC,
    OTHER_CODEC = 0,
};

enum StreamType
{
    STREAM_TYPE_AUDIO_AAC = 0x0F,  // Audio, MPEG-2 AAC, AAC with adts format, common
    STREAM_TYPE_VIDEO_H264 = 0x1B, // Video, H.264 or AVC
    STREAM_TYPE_VIDEO_H265 = 0x24, // Video, H.265 or HEVC
};

static const char* CODEC[] = {
    "unknown",
    "avc",
    "hevc",
    "aac",
};

#define BUF_SIZE 1024 * 1024
#define RB16(x)  ((uint16_t)(((const uint8_t *)(x))[0] << 8) | ((const uint8_t *)(x))[1])

class TsFileParser
{
public:
    TsFileParser(std::string file_name)
        : m_file(file_name), m_fd(-1), m_parsed(0), m_file_size(0), m_rest_bytes(0), m_program_num(0) {};
    void Start();
    int  ParseTsPacket(uint8_t* data, int data_len);
    int  ParsePat(uint8_t* data, int data_len);
    int  ParsePmt(uint8_t* data, int data_len);
    bool IsPmt(int pid);

private:
    int m_fd;
    long long m_parsed;
    int m_rest_bytes;
    long long m_file_size;
    // int m_last_pes_start;
    int m_program_num;
    int m_pmt_id[16];

    std::map<uint16_t, int> m_streams;

    uint8_t     m_last_rest[188];
    uint8_t     m_buf[BUF_SIZE];
    std::string m_file;
};

void TsFileParser::Start()
{
    do
    {
        if (access(m_file.c_str(), F_OK) != 0)
        {
            printf("[%s] not exist\n", m_file.c_str());
            break;
        }

        struct stat buf;
        int         result = stat(m_file.c_str(), &buf);
        if (result == -1)
        {
            printf("[%s] get file size failed\n", m_file.c_str());
            break;
        }
        m_file_size = buf.st_size;

        m_fd = open(m_file.c_str(), O_RDWR | O_LARGEFILE);
        if (m_fd == -1)
        {
            printf("[%s] open failed\n", m_file.c_str());
            break;
        }

        memset(m_pmt_id, 0, 16);

        while (m_parsed < m_file_size)
        {
            if (m_rest_bytes)
            {
                memcpy(m_buf, m_last_rest, m_rest_bytes);
            }

            int n = read(m_fd, m_buf + m_rest_bytes, BUF_SIZE - m_rest_bytes);
            if (n <= 0)
            {
                printf("[%s] read failed, parsed: %d\n", m_file.c_str(), m_parsed);
                break;
            }
	    n += m_rest_bytes;
            m_rest_bytes = 0;

            int parsed = 0;
            while (parsed < n)
            {
                if (m_buf[parsed] != 0x47)
                {
                    parsed++;
                    continue;
                }

                if (n - parsed < 188)
                {
                    m_rest_bytes = n - parsed;
                    memcpy(m_last_rest, m_buf + parsed, m_rest_bytes);
                    parsed = n;
                }
                else
                {
                    ParseTsPacket(m_buf + parsed, 188);
                    parsed += 188;
                    m_parsed += 188;
                }
            }
        }

        printf("[%s] end, parsed: %d, file_size: %d\n", m_file.c_str(), m_parsed, m_file_size);
    } while (0);
}

int TsFileParser::ParseTsPacket(uint8_t* data, int data_len)
{
    int      offset = 4;
    uint16_t t = RB16(data + 1);
    uint8_t  c = data[3];

    int     pid = t & 0x1FFF;
    uint8_t payload_unit_start_indicator = (t >> 14) & 0x01;
    uint8_t adaption_field_control = (c >> 4) & 0x03;
    uint8_t adaption_field_length = 0;
    if (adaption_field_control == 2 || adaption_field_control == 3)
    {
        adaption_field_length = data[4];
        offset += adaption_field_length + 1;
    }

    if (adaption_field_control == 1 || adaption_field_control == 3)
    {
        assert(data_len > offset);
        if (pid == 0)
        {
            printf("[%s-%d-%d] packet pid: %d, type: pat\n", m_file.c_str(), m_parsed, m_parsed / 188, pid);
            int re = ParsePat(data + offset, data_len - offset);
            if (re == -1)
            {
                // TODO
            }
        }
        else if (IsPmt(pid))
        {
            printf("[%s-%d-%d] packet pid: %d, type: pmt\n", m_file.c_str(), m_parsed, m_parsed / 188, pid);
            int re = ParsePmt(data + offset, data_len - offset);
            if (re == -1)
            {
                // TODO
            }
        }
        else if (m_streams.count(pid))
        {
            if (payload_unit_start_indicator)
            {
                printf("[%s-%d-%d] packet pid: %d, type: pes(%s), is pes header\n", m_file.c_str(), m_parsed,
                    m_parsed / 188, pid, CODEC[m_streams[pid]]);
            }
            else
            {
                printf("[%s-%d-%d] packet pid: %d, type: pes(%s)\n", m_file.c_str(), m_parsed, m_parsed / 188, pid,
                    CODEC[m_streams[pid]]);
            }
        }
        else
        {
            printf("[%s-%d-%d] packet pid: %d, type: unknown\n", m_file.c_str(), m_parsed, m_parsed / 188, pid);
        }
    }
}

int TsFileParser::ParsePat(uint8_t* data, int data_len)
{
    uint16_t s = RB16(data + 2);
    uint16_t section_length = s & 0x3FF;
    uint16_t program_bytes = section_length - 5 - 4;
    m_program_num = program_bytes / 4;
    if (!m_program_num)
    {
        // TODO
    }
    assert(m_program_num < 16);
    for (uint16_t j = 0; j < program_bytes; j += 4)
    {
        uint16_t program_number = RB16(data + 1 + 8 + j);
        if (program_number == 0)
        {
            // network_PID
        }
        else
        {
            if (!m_pmt_id[j / 4])
            {
                m_pmt_id[j / 4] = RB16(data + 1 + 8 + j + 2) & 0x1FFF;
            }
        }
    }
}

int TsFileParser::ParsePmt(uint8_t* data, int data_len)
{
    uint16_t s = RB16(data + 1 + 1);
    uint16_t section_length = s & 0x3FF;

    int16_t  p = RB16(data + 1 + 10);
    uint16_t program_info_length = p & 0x3FF;

    uint16_t stream_info_bytes = section_length - program_info_length - 9 - 4;
    uint16_t offset = 1 + 12 + program_info_length;
    uint16_t parsed = 0;

    while (parsed < stream_info_bytes)
    {
        uint16_t e = RB16(data + offset + parsed + 1);
        uint16_t stream_pid = e & 0x1FFF;
        uint16_t stream_type = data[offset + parsed];

        if (!m_streams.count(stream_pid))
        {
            if (stream_type == STREAM_TYPE_AUDIO_AAC)
            {
                m_streams[stream_pid] = AAC;
            }
            else if (stream_type == STREAM_TYPE_VIDEO_H264)
            {
                m_streams[stream_pid] = AVC;
            }
            else if (stream_type == STREAM_TYPE_VIDEO_H265)
            {
                m_streams[stream_pid] = HEVC;
            }
            else
            {
                m_streams[stream_pid] = OTHER_CODEC;
            }
        }

        uint16_t i = RB16(data + offset + parsed + 3);
        uint16_t ES_info_length = i & 0x0FFF;
        parsed += ES_info_length + 5;
    }
}

bool TsFileParser::IsPmt(int pid)
{
    bool re = false;
    for (int i = 0; i < m_program_num; i++)
    {
        if (pid == m_pmt_id[i])
        {
            re = true;
        }
    }

    return re;
}

int main(int argc, char* argv[])
{
    if (argc == 2)
    {
        std::string   file_path = argv[1];
        TsFileParser* ts_parser = new TsFileParser(file_path);
        ts_parser->Start();
    }
    else
    {
        // TODO
    }
    return 0;
}

