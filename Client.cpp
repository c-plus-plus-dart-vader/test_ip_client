//#define _GNU_SOURCE 
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

using namespace std;

constexpr string_view tcp_proto{"TCP"};
constexpr string_view udp_proto{"UDP"};
constexpr size_t sv_npos = string_view::npos;
constexpr size_t READ_BUFFER_SIZE = 20;
constexpr string_view UDP_PACKET_BEGIN {"proteyclient"};
//UDP_PACKET_BEGIN(12 bytes) + PACKETS_QTY(2 bytes) + PACKET_NUMBER(2 bytes) + PACKET_SIZE(2 bytes)
constexpr uint8_t UDP_PH_QTY_POS = UDP_PACKET_BEGIN.size();
constexpr uint8_t UDP_PH_SEQ_NUM_POS = UDP_PH_QTY_POS + 2;
constexpr uint8_t UDP_PH_SIZE_POS = UDP_PH_SEQ_NUM_POS + 2;
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
	if(errno == EPIPE)
	{
		cout<<"Connection is broken\n";
		close(m_desc);
		m_desc = -1;
		m_is_started = false;
		return Res_e::CONNECTION_BROKEN;	
	}
	else
	{
		cout<<(IsWrite ? "Write" : "Read")<<" to server failed: "<<strerror(errno)<<"\n";
		return Res_e::FAILURE;
	}
}

Client::~Client()
{
	cout<<"Client DTOR\n";
	if (-1 == m_desc) close(m_desc);
}		

Client::Res_e Client::Start(std::string_view params)
{
	if (m_is_started) return Res_e::ALREADY_STARTED;
	
	if (auto const res = ValidateInputParams(params); Res_e::SUCCESS != res) { return res; }
	
	return Start();
}
	
Client::Res_e Client::Start()
{
	cout<<((m_proto == IPPROTO_TCP) ? tcp_proto : udp_proto)<<" protocol will be used with socket\n";
	
	m_desc = socket(AF_INET, (m_proto == IPPROTO_TCP) ? SOCK_STREAM : SOCK_DGRAM, m_proto);
	if (-1 == m_desc)
	{
		cout<<"Creation of an unbound socket and get file descriptor failed: "<<strerror(errno)<<"\n";
		if ((errno == ENFILE)or(errno == EMFILE)or(errno == ENOBUFS)or(errno == ENOMEM))
		{
			cout<<"You can try to create socket later\n";
			return Res_e::TEMPORARY_UNSUFFICIENT_RESOURCES;
		}
		return Res_e::FAILURE;
	}
	
	if (IPPROTO_TCP == m_proto)
	{	
		if (-1 == connect(m_desc, (sockaddr const*)&m_server_sa, sizeof(m_server_sa)))
		{
			cout<<"Connect to TCP server failed: "<<strerror(errno)<<"\n";
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
						
						cout<<"Client connected to TCP server\n";
						m_is_started = true;
						return Res_e::SUCCESS;
					}
					else//0 value(timer expired) is impossible because third param in poll call is -1
					{
						if (errno != EINTR) { cout<<"Connect to TCP server failed after EINTR: "<<strerror(errno)<<"\n"; break; }
						pfd.revents = 0;
					}
				}				
				
			}
			close(m_desc);
			m_desc = -1;
			return Res_e::FAILURE; 
		}
		
		cout<<"Client connected to TCP server\n";
	}
	
	m_is_started = true;
	return Res_e::SUCCESS;
}

Client::Res_e Client::SendMsg(std::string const& msg)
{
	if (not m_is_started) return Res_e::NOT_STARTED;
	if (msg.empty()) return Res_e::NO_DATA_TO_SEND;
	cout<<"Request message size is "<<msg.size()<<" bytes\n";
	
	if (IPPROTO_TCP == m_proto)
	{
		while(1)
		{
			auto written_bytes = write(m_desc, msg.data(), msg.size());
			if (-1 == written_bytes)
			{
				if (errno == EINTR)
				{
					cout<<"EINTR is received.Try again send message\n";
					continue;
				}
				return ErrorHandlingExceptEINTR(true);
			}
			else
			{
				cout<<"Write "<<written_bytes<<" bytes\n";
				if (written_bytes != msg.size())
				{
					cout<<"Not all data was sent\n";
					return Res_e::FAILURE;
				}
				break;
			}
		}
		
		char end = '\n';
		write(m_desc, &end, 1);
		cout<<"Write END byte\n";
		
		char buffer[READ_BUFFER_SIZE];
		m_last_received_answer.clear();
		do 
		{ 
			size_t read_bytes;
			if (IPPROTO_TCP == m_proto)
			{
				read_bytes = read(m_desc, buffer, READ_BUFFER_SIZE);
			}

			if (-1 == read_bytes)
			{
				if (errno == EINTR)
				{
					cout<<"EINTR is received.Try again read message\n";
					continue;
				}
				return ErrorHandlingExceptEINTR(false);
			}
			m_last_received_answer.append(buffer, read_bytes);
			cout<<"Read "<<read_bytes<<" bytes\n";
		} while (m_last_received_answer.back() != '\n');
	}
	else
	{
		//UDP_PACKET_BEGIN(12 bytes) + PACKETS_QTY(2 bytes) + PACKET_NUMBER(2 bytes) + PACKET_SIZE(2 bytes)
		uint16_t constexpr packet_size = 64;
		uint16_t packet_qty = msg.size()/(packet_size - UDP_PACKET_HEADER_SIZE);
		uint16_t ost = msg.size()%(packet_size - UDP_PACKET_HEADER_SIZE);
		if (ost > 0){ ++packet_qty; }
		for (size_t i = 0; i < packet_qty; ++i)
		{
			char wbuff[packet_size];
			memcpy(wbuff, UDP_PACKET_BEGIN.data(), UDP_PACKET_BEGIN.size());
			wbuff[UDP_PH_QTY_POS] = (packet_qty&0xff00)>>8;
			wbuff[UDP_PH_QTY_POS + 1] = packet_qty&0xff;
			
			wbuff[UDP_PH_SEQ_NUM_POS] = (i&0xff00)>>8;
			wbuff[UDP_PH_SEQ_NUM_POS + 1] = i&0xff;
			
			wbuff[UDP_PH_SIZE_POS] = (packet_size&0xff00)>>8;
			wbuff[UDP_PH_SIZE_POS + 1] = packet_size&0xff;
			
			size_t payload_len;
			if ((packet_qty - 1) == i and ost > 0) payload_len = ost;
			else payload_len = packet_size - UDP_PACKET_HEADER_SIZE;
			
			memcpy(&wbuff[UDP_PACKET_HEADER_SIZE], &msg[i*(packet_size - UDP_PACKET_HEADER_SIZE)], payload_len);
			
			socklen_t len = sizeof(m_server_sa);
			sendto(m_desc, wbuff, UDP_PACKET_HEADER_SIZE + payload_len, 0, (sockaddr const*)&m_server_sa, len);
		}
		
		uint16_t rd_packets_qty;
		uint16_t rd_packet_size;
		char buffer [MAX_UDP_PACKET_SIZE];
		vector<size_t> packet_numbers;
		do
		{
			socklen_t len = sizeof(m_server_sa);
			auto read_bytes = recvfrom(m_desc, buffer, MAX_UDP_PACKET_SIZE, 0, (sockaddr*)&m_server_sa, &len);
			if (read_bytes)
			{
				cout<<"Read "<<read_bytes<<" bytes\n";

				if (read_bytes < (UDP_PACKET_HEADER_SIZE + 1))
				{
					cout<<"Packet is not from protey client\n";
					continue;
				}
				if (string_view{buffer, UDP_PACKET_BEGIN.size()} != UDP_PACKET_BEGIN)
				{
					cout<<"Packet is not from protey server\n";
					continue;
				}
				
				if (packet_numbers.empty())
				{
					rd_packets_qty = ((uint16_t)buffer[UDP_PH_QTY_POS]<<8)|(uint16_t)buffer[UDP_PH_QTY_POS + 1];
					cout<<"Packet qty is "<<rd_packets_qty<<"\n";
					rd_packet_size = ((uint16_t)buffer[UDP_PH_SIZE_POS]<<8)|(uint16_t)buffer[UDP_PH_SIZE_POS + 1];
					cout<<"Packet size is "<<rd_packet_size<<"\n";
					m_last_received_answer.resize(rd_packets_qty*(rd_packet_size - UDP_PACKET_HEADER_SIZE));
				}
				uint16_t rd_packet_number = ((uint16_t)buffer[UDP_PH_SEQ_NUM_POS]<<8)|(uint16_t)buffer[UDP_PH_SEQ_NUM_POS + 1];
				cout<<"Packet number is "<<rd_packet_number<<"\n";
				if (find(packet_numbers.begin(), packet_numbers.end(), rd_packet_number) != packet_numbers.end())
				{
					cout<<"Duplicated packet\n";
					continue;
				}
				
				if (rd_packet_number != (rd_packets_qty - 1))//not the last packet of message
				{
					if (read_bytes != rd_packet_size)
					{
						cout<<"Damaged packet\n";
						continue;
					}
				}
				else
				{
					m_last_received_answer.resize((rd_packets_qty - 1)*(rd_packet_size - UDP_PACKET_HEADER_SIZE) + read_bytes - UDP_PACKET_HEADER_SIZE);
				}
				packet_numbers.push_back(rd_packet_number);
				memcpy(&m_last_received_answer[rd_packet_number*(rd_packet_size - UDP_PACKET_HEADER_SIZE)], &buffer[UDP_PACKET_HEADER_SIZE], read_bytes - UDP_PACKET_HEADER_SIZE);
			}
			if (-1 == read_bytes)
			{
				if (errno == EINTR)
				{
					cout<<"EINTR is received.Try again read message\n";
					continue;
				}
				return ErrorHandlingExceptEINTR(false);
			}
		}while(rd_packets_qty != packet_numbers.size());
	}
	
	cout<<"Answer:\n"<<m_last_received_answer;
	if (m_last_received_answer.back() != '\n') cout<<'\n';
	return Res_e::SUCCESS;
}
