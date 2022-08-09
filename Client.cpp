#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>
#include <charconv>
#include <poll.h>
#include <vector>
#include <algorithm>
#include "Client.hpp"
#include "Logger.hpp"

using namespace std;

template<class ... Args>
void Log(Args&& ... args)
{
	utils::Log("Client.cpp: ", forward<Args>(args)...);
}

constexpr string_view      UDP_PACKET_BEGIN {"proteyclient"};

constexpr string_view      tcp_proto{"TCP"};
constexpr string_view      udp_proto{"UDP"};

constexpr size_t sv_npos = string_view::npos;
constexpr size_t TCP_READ_BUFFER_SIZE = 25;


constexpr uint16_t UDP_PH_QTY_POS         = UDP_PACKET_BEGIN.size();
constexpr uint16_t UDP_PH_SEQ_NUM_POS     = UDP_PH_QTY_POS + 2;
constexpr uint16_t UDP_PH_SIZE_POS        = UDP_PH_SEQ_NUM_POS + 2;
constexpr uint16_t UDP_PACKET_HEADER_SIZE = UDP_PH_SIZE_POS + 2;

Client::Res_e Client::ValidateInputParams(std::string_view params)
{
	auto const pos1 = params.find_first_of(",");
	if (pos1 == sv_npos or pos1 == (params.size() - 1))
	{
		return Res_e::NOT_ALL_INPUT_PARAMETERS_ARE_SET;
	}
	auto const pos2 = params.find_first_of(",", pos1 + 1);
	if (pos2 == sv_npos)
	{
		return Res_e::NOT_ALL_INPUT_PARAMETERS_ARE_SET;
	}
	auto const proto = params.substr(0, pos1); 
	if (proto == tcp_proto) m_proto = IPPROTO_TCP;
	else if (proto == udp_proto) m_proto = IPPROTO_UDP;
	else return Res_e::INVALID_PROTOCOL_INPUT_PARAMETER;

	auto const ip_len = pos2 - pos1 - 1;
	if (ip_len > MAX_IPv4_SIZE) return Res_e::INVALID_IPv4_INPUT_PARAMETER;
	if (params.size() - pos2 - 1 > sizeof(65535)) return Res_e::INVALID_PORT_INPUT_PARAMETER;
	
	memset(&m_server_sa, 0, sizeof(m_server_sa));
	
	char ip_term[MAX_IPv4_SIZE + 1];
	memcpy(ip_term, &params[pos1 + 1], ip_len);
	ip_term[ip_len] = '\0';
	if (1 != inet_pton(AF_INET, ip_term, &m_server_sa.sin_addr)) return Res_e::INVALID_IPv4_INPUT_PARAMETER;
	
	uint16_t server_port;
	if (from_chars(&params[pos2 + 1], params.end(), server_port).ptr != params.end()) return Res_e::INVALID_PORT_INPUT_PARAMETER;
	m_server_sa.sin_port = htons(server_port);
	
	m_server_sa.sin_family = AF_INET;
	
	return Res_e::SUCCESS;
}

Client::Res_e Client::ErrorHandlingExceptEINTR(bool IsWrite)
{
	if(errno == ECONNRESET)
	{
		Log("Connection is broken and socket ", m_desc, " will be closed");
		close(m_desc);
		m_desc = -1;
		m_is_started = false;
		return Res_e::CONNECTION_BROKEN;	
	}
	Log(IsWrite ? "Write" : "Read", " to server failed: ", strerror(errno));
	if ((errno == ENOBUFS) or (errno == ENOMEM)) {
		return Res_e::TEMPORARY_UNSUFFICIENT_RESOURCES;
	}
	else {
		return Res_e::FAILURE;
	}
}

Client::~Client()
{
	Log("DTOR");
	if (-1 != m_desc)
	{
		close(m_desc);
		Log(m_proto == IPPROTO_TCP ? tcp_proto : udp_proto, " socket ", m_desc, " was closed");
	}
}		

Client::Res_e Client::Start(std::string_view params)
{
	if (m_is_started) return Res_e::ALREADY_STARTED;
	
	if (auto const res = ValidateInputParams(params); Res_e::SUCCESS != res) { return res; }
	
	return Start();
}
	
Client::Res_e Client::Start()
{
	m_desc = socket(AF_INET, (m_proto == IPPROTO_TCP) ? SOCK_STREAM : SOCK_DGRAM, m_proto);
	if (-1 == m_desc)
	{
		Log("Creation of an unbound socket and get file descriptor failed: ", strerror(errno));
		if ((errno == ENFILE)or(errno == EMFILE)or(errno == ENOBUFS)or(errno == ENOMEM))
		{
			Log("You can try to create socket later");
			return Res_e::TEMPORARY_UNSUFFICIENT_RESOURCES;
		}
		return Res_e::FAILURE;
	}
	Log(IPPROTO_TCP == m_proto ? tcp_proto : udp_proto, " socket ", m_desc, " is created");
	
	if (IPPROTO_TCP == m_proto)
	{	
		if (-1 == connect(m_desc, (sockaddr const*)&m_server_sa, sizeof(m_server_sa)))
		{
			Log("Connect to TCP server failed: ", strerror(errno));
			if (errno == EINTR)
			{
				pollfd pfd;
				pfd.fd = m_desc;
				pfd.events = POLLOUT;
				while(1)
				{
					int res = poll(&pfd, 1, -1);
					if (1 == res)
					{
						if (not pfd.revents&POLLOUT or pfd.revents&POLLERR){ break; }

						socklen_t len = sizeof(res);
						if (-1 == getsockopt(m_desc, SOL_SOCKET, SO_ERROR, &res, &len)) { break; }
						if (res != 0) { break; } 
						
						Log("Socket ", m_desc, " connected to TCP server");
						m_is_started = true;
						return Res_e::SUCCESS;
					}
					else//0 value(timer expired) is impossible because third param in poll call is -1
					{
						if (errno != EINTR) {
							Log("Connect to TCP server failed after EINTR: ", strerror(errno));
							break;
						}
						pfd.revents = 0;
					}
				}				
				
			}
			Log("Socket ", m_desc, " will be closed");
			close(m_desc);
			m_desc = -1;
			return Res_e::FAILURE; 
		}
		
		Log("Socket ", m_desc, " connected to TCP server");
	}
	
	m_is_started = true;
	return Res_e::SUCCESS;
}

Client::Res_e Client::SendMsg(std::string const& msg)
{
	if (not m_is_started) return Res_e::NOT_STARTED;
	if (msg.empty()) return Res_e::NO_DATA_TO_SEND;
	Log("Socket ", m_desc, " starts sending request with size ", msg.size(), " bytes");
	
	if (IPPROTO_TCP == m_proto)
	{
		size_t start_pos = 0;
		size_t len_to_send = msg.size();
		while(msg.size() != start_pos)
		{
			int written_bytes = send(m_desc, msg.data() + start_pos, len_to_send, 0);
			if (-1 == written_bytes)
			{
				if (errno == EINTR)
				{
					Log("EINTR is received.Try again send message");
					continue;
				}
				return ErrorHandlingExceptEINTR(true);
			}
			else
			{
				Log("Socket ", m_desc, " write ", written_bytes, " bytes");
				start_pos += written_bytes;
				len_to_send = msg.size() - start_pos;
			}
		}
		
		char end = '\n';
		while(1)
		{
			if (-1 == send(m_desc, &end, 1, 0))
			{
				if (errno == EINTR)
				{
					Log("EINTR is received.Try again send message");
					continue;
				}
				return ErrorHandlingExceptEINTR(true);	
			}
			break;
		}
		Log("Socket ", m_desc, " write END byte");
		
		char buffer[TCP_READ_BUFFER_SIZE];
		m_last_received_answer.clear();
		while(1) 
		{ 
			int read_bytes = read(m_desc, buffer, TCP_READ_BUFFER_SIZE);

			if (-1 == read_bytes)
			{
				if (errno == EINTR)
				{
					Log("EINTR is received.Try again read message");
					continue;
				}
				return ErrorHandlingExceptEINTR(false);
			}
			else
			{
				Log("Socket ", m_desc, " read ", read_bytes, " bytes");	
				m_last_received_answer.append(buffer, read_bytes);
			}
			
			if ('\n' == m_last_received_answer.back())
			{
				if (isdigit(m_last_received_answer[0])) {
					m_last_received_answer[m_last_received_answer.rfind('\t')] = '\n';
				}
				break;
			}
		}
		//remove '\n' because it does not present in original request
		m_last_received_answer.pop_back();
	}
	else
	{
		//UDP_PACKET_BEGIN(12 bytes) + PACKETS_QTY(2 bytes) + PACKET_NUMBER(2 bytes) + PACKET_SIZE(2 bytes)
		uint16_t constexpr packet_size = 64;
		uint16_t packets_qty = msg.size()/(packet_size - UDP_PACKET_HEADER_SIZE);
		uint16_t ost = msg.size()%(packet_size - UDP_PACKET_HEADER_SIZE);
		if (ost > 0){ ++packets_qty; }
		Log("Packets qty is ", packets_qty);
		size_t i = 0;
		for (; i < packets_qty; )
		{
			char wbuff[packet_size];
			memcpy(wbuff, UDP_PACKET_BEGIN.data(), UDP_PACKET_BEGIN.size());
			uint16_t packets_qty_net = htons(packets_qty);
			memcpy(wbuff + UDP_PH_QTY_POS, &packets_qty_net, sizeof(packets_qty_net));
			uint16_t seq_num_net = htons(i);
			memcpy(wbuff + UDP_PH_SEQ_NUM_POS, &seq_num_net, sizeof(seq_num_net));
			uint16_t ps_net = htons(packet_size);
			memcpy(wbuff + UDP_PH_SIZE_POS, &ps_net, sizeof(ps_net));

			size_t payload_len;
			if ((packets_qty - 1) == i and ost > 0) payload_len = ost;
			else payload_len = packet_size - UDP_PACKET_HEADER_SIZE;
			
			memcpy(&wbuff[UDP_PACKET_HEADER_SIZE], &msg[i*(packet_size - UDP_PACKET_HEADER_SIZE)], payload_len);
			
			socklen_t len = sizeof(m_server_sa);
			int written_bytes = sendto(m_desc, wbuff, UDP_PACKET_HEADER_SIZE + payload_len, 0, (sockaddr const*)&m_server_sa, len);
			if (written_bytes >= 0)
			{
				Log("Socket ", m_desc, " write ", written_bytes, " bytes");
				if (written_bytes != (UDP_PACKET_HEADER_SIZE + payload_len)) {
					Log("Socket ", m_desc, " will repeat this packet sending");
					continue;
				}
				++i;
			}
			else {
				if (errno == EINTR)
				{
					Log("EINTR is received.Try again read message");
					continue;
				}
				Log("Write failed: ", strerror(errno));
				return Res_e::FAILURE;
			}
		}
		
		uint16_t rd_packets_qty;
		char buffer [MAX_UDP_PACKET_SIZE];
		vector<size_t> packet_numbers;
		do
		{
			socklen_t len = sizeof(m_server_sa);
			int read_bytes = recvfrom(m_desc, buffer, MAX_UDP_PACKET_SIZE, 0, (sockaddr*)&m_server_sa, &len);
			if (read_bytes > 0)
			{
				Log("Socket ", m_desc, " read ", read_bytes, " bytes");

				if (read_bytes < UDP_PACKET_HEADER_SIZE)
				{
					Log("Packet is not from protey client");
					continue;
				}
				if (string_view{buffer, UDP_PACKET_BEGIN.size()} != UDP_PACKET_BEGIN)
				{
					Log("Packet is not from protey server");
					continue;
				}
				
				uint16_t rd_packet_number;
				memcpy(&rd_packet_number, buffer + UDP_PH_SEQ_NUM_POS, sizeof(rd_packet_number));
				rd_packet_number = ntohs(rd_packet_number);
				Log("Packet number is ", rd_packet_number);
				
				if (find(packet_numbers.begin(), packet_numbers.end(), rd_packet_number) != packet_numbers.end())
				{
					Log("Duplicated packet");
					continue;
				}
				
				if (packet_numbers.empty())
				{
					memcpy(&rd_packets_qty, buffer + UDP_PH_QTY_POS, sizeof(rd_packets_qty));
					rd_packets_qty = ntohs(rd_packets_qty);
					Log("Packet qty is ", rd_packets_qty);
					m_last_received_answer.resize(rd_packets_qty*(packet_size - UDP_PACKET_HEADER_SIZE));
				}
				
				if (rd_packet_number != (rd_packets_qty - 1))//not the last packet of message
				{
					if (read_bytes != packet_size)
					{
						Log("Damaged packet");
						continue;
					}
				}
				else
				{
					m_last_received_answer.resize((rd_packets_qty - 1)*(packet_size - UDP_PACKET_HEADER_SIZE) + read_bytes - UDP_PACKET_HEADER_SIZE);
				}
				packet_numbers.push_back(rd_packet_number);
				memcpy(&m_last_received_answer[rd_packet_number*(packet_size - UDP_PACKET_HEADER_SIZE)], &buffer[UDP_PACKET_HEADER_SIZE], read_bytes - UDP_PACKET_HEADER_SIZE);
			}
			else if (0 == read_bytes)
			{
				Log("Socket read 0 bytes");
				continue;
			}
			else
			{
				if (errno == EINTR)
				{
					Log("EINTR is received.Try again read message");
					continue;
				}
				return ErrorHandlingExceptEINTR(false);
			}
		}while(rd_packets_qty != packet_numbers.size());
	}
	
	Log("Answer:\n", m_last_received_answer);
	
	return Res_e::SUCCESS;
}
