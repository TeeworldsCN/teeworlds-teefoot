/* CWebapp Class by Sushi */
#ifndef GAME_SERVER_WEBAPP_H
#define GAME_SERVER_WEBAPP_H

#include <string>
#include <base/tl/array.h>
#include "gamecontext.h"

class CWebapp
{
	CGameContext *m_pGameServer;
	NETADDR m_Addr;
	NETSOCKET m_Socket;
	
	array<std::string> m_lMapList;
	
	CGameContext *GameServer() { return m_pGameServer; }
	
	bool Connect();
	void Disconnect();
	std::string SendAndReceive(const char* pString);
	
public:
	CWebapp(CGameContext *pGameServer);
	~CWebapp();
	
	bool PingServer();
	void LoadMapList();
	void UserAuth();
};

// this is to buffer all the data
struct CWenappData
{
	char m_aUsername[32];
	char m_aPassword[32];
	int m_Time;
};

#endif
