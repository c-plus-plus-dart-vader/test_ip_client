#include <iostream>
#include <string>
#include "Client.hpp"

int main(){
	std::cout<<"Specify protocol(TCP/UDP),server IPv4 address and port like(tcp,10.10.10.10,5555)\n";
	std::string params;
	std::cin>>params;
	Client client;
	if (Client::Res_e::SUCCESS != client.Start(params))
	{
		return 0;
	}
	
	std::string s;
	while(1)
	{
		std::cout<<"Input string\n";
		std::getline(std::cin, s);
		auto res = client.SendMsg(s);
		while (Client::Res_e::SUCCESS != res && Client::Res_e::NO_DATA_TO_SEND != res)
		{
			if (Client::Res_e::CONNECTION_BROKEN == res)
			{
				if (Client::Res_e::SUCCESS != client.Start())
				{
					return 0;
				}
			}
			res = client.SendMsg(s);
		}
		
	}
	return 0;
};
