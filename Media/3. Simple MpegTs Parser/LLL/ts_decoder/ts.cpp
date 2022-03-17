#include <set>
#include <map>

#include "ts.h"

typedef struct stream_type {
	int id;
	const char* desc;
} stream_type_t;

static stream_type_t stream_types[] = {
	{0x0f, "aac"},
	{0x1b, "h264"},
	{0x24, "hevc"}
};

static const char* stream2string(int stream) {
	for (int i = 0; i < sizeof(stream_types) / sizeof(stream_type_t); i++) {
		if (stream_types[i].id == stream) {
			return stream_types[i].desc;
		}
	}
	return "unknown";
}

void print_title() {
	printf("%-10s  %-6s %-6s\n", "offset", "pid", "type");
}

void print_data(int offset, int pid, const std::string& type)
{
	printf("0x%08x  %-6d %-6s\n", offset, pid, type.c_str());
}

TS::TS()
{
}

TS::~TS()
{
}

void TS::decode(Stream& s)
{
	std::set<int> pmt_pids;
	std::map<int, int> stream_types;

	print_title();

	while (s.next_byte() == TS_SYNC_BYTE)
	{
		int start_pos = s.pos();

		// 8 bits sync_byte
		s.skip(1);

		// 1 bit trasport_error_indicator
		// 1 bit payload_unit_start_indicator
		// 1 bit transport_priority
		// 13 bits pid
		uint16_t bytes = s.read_2bytes();
		int payload_unit_start_indicator = (bytes >> 14) & 0x01;
		int pid = (bytes >> 0) & 0x1fff;

		// 2 bits transport_scrambling_control
		// 2 bits adaptation_field_control
		// 4 bits continuity_counter
		bytes = s.read_1byte();
		int adaptation_field_control = (bytes >> 4) & 0x03;
		if (pid == TS_PAT_PID) {

			print_data(start_pos, pid, "pat");

			pmt_pids.clear();

			int pointer_field = 0;
			if (payload_unit_start_indicator) {
				// 8 bits pointer_field
				pointer_field = s.read_1byte();
			}
			s.skip(pointer_field);

			// 8 bits table_id
			s.skip(1);

			// 1 bit section_syntax_indicator
			// 1 bit '0'
			// 2 bits reserved
			// 12 bits section_length
			uint16_t bytes = s.read_2bytes();
			int section_length = (bytes >> 0) & 0x0fff;

			// 16 bits transport_stream_id
			// 2 bits reserved
			// 5 bits version_number
			// 1 bit current_next_indicator
			// 8 bits section_number
			// 8 bits last_section_number
			s.skip(5);

			int program_bytes = section_length - 5 - 4;
			for (int i = 0; i < program_bytes; i += 4) {
				// 16 bits program_number
				// 3 bits reserved
				// 13 bits network_PID or program_map_PID
				uint32_t bytes = s.read_4bytes();
				int pmt_pid = (bytes >> 0) & 0x1fff;
				pmt_pids.insert(pmt_pid);
			}

			// 32 bits crc
			s.skip(4);

			s.skip(TS_PACKET_SIZE - (s.pos() - start_pos));
		}
		else if (pmt_pids.find(pid) != pmt_pids.end())
		{

			print_data(start_pos, pid, "pmt");

			stream_types.clear();

			int pointer_field = 0;
			if (payload_unit_start_indicator) {
				// 8 bits pointer_field
				pointer_field = s.read_1byte();
			}

			s.skip(pointer_field);

			// 8 bits table_id
			s.skip(1);

			// 1 bit section_syntax_indicator
			// 1 bit '0'
			// 2 bits reserved
			// 12 bits section_length
			uint16_t bytes = s.read_2bytes();
			int section_length = (bytes >> 0) & 0x0fff;

			// 16 bits program_number
			// 2 bits reserved
			// 5 bits version_number
			// 1 bit current_next_indicator
			// 8 bits section_number
			// 8 bits last_section_number
			// 3 bits reserved
			// 13 bits PCR_PID
			s.skip(7);

			// 4 bits reserved
			// 12 bits program_info_length
			bytes = s.read_2bytes();
			int program_info_length = (bytes >> 0) & 0x3ff;
			s.skip(program_info_length);

			int es_bytes = section_length - 9 - 4;
			for (int i = 0; i < es_bytes;) {
				// 8 bits stream_type
				int stream_type = s.read_1byte();

				// 3 bits reserved
				// 13 bits elementary_PID
				uint16_t bytes = s.read_2bytes();
				int pid = (bytes >> 0) & 0x1fff;

				// 4 bits reserved
				// 12 bits ES_info_length
				bytes = s.read_2bytes();
				int es_info_length = (bytes >> 0) & 0x3ff;
				s.skip(es_info_length);

				stream_types[pid] = stream_type;

				i += (5 + es_info_length);
			}

			// 32 bits crc
			s.skip(4);

			s.skip(TS_PACKET_SIZE - (s.pos() - start_pos));
		}
		else if (stream_types.find(pid) != stream_types.end()) {

			print_data(start_pos, pid, stream2string(stream_types[pid]));

			s.skip(TS_PACKET_SIZE - (s.pos() - start_pos));
		}
		else {

			print_data(start_pos, pid, "unknown");

			s.skip(TS_PACKET_SIZE - (s.pos() - start_pos));
		}
	}
}
