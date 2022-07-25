#pragma once

#include <arpa/inet.h>
#include <string_view>

class Client
{
	static constexpr uint8_t MAX_IPv4_SIZE = 15;
	static constexpr std::size_t MAX_UDP_PACKET_SIZE = 1024;
public:
	enum class Res_e : uint8_t
	{
		ALREADY_STARTED,
		NOT_STARTED,
		NOT_ALL_INPUT_PARAMETERS_ARE_SET,
		INVALID_PROTOCOL_INPUT_PARAMETER,
		INVALID_IPv4_INPUT_PARAMETER,
		INVALID_PORT_INPUT_PARAMETER,
		TEMPORARY_UNSUFFICIENT_RESOURCES,
		FAILURE,
		CONNECTION_BROKEN,
		NO_DATA_TO_SEND,
		SUCCESS
	};
	~Client();
	Res_e Start(std::string_view params);
	Res_e Start();
	Res_e SendMsg(std::string const& msg);
	std::string const& GetLastReceivedAnswer() const { return m_last_received_answer; }
private:
	Res_e ValidateInputParams(std::string_view params);
	Res_e ErrorHandlingExceptEINTR(bool IsWrite);
	
	int         m_proto;
	int         m_desc{-1};
	sockaddr_in m_server_sa;
	bool        m_is_started{false};
	std::string m_last_received_answer;
};
