/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_GAMEMODES_FOOT_H
#define GAME_SERVER_GAMEMODES_FOOT_H
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

// you can subclass GAMECONTROLLER_CTF, GAMECONTROLLER_TDM etc if you want
// todo a modification with their base as well.
class CGameControllerFoot : public IGameController
{
	void Reset();
public:
	CGameControllerFoot(class CGameContext *pGameServer);
	virtual void Tick();
	CPlayer *m_apPlayers[MAX_CLIENTS];

	virtual void Snap(int SnappingClient);
	int OnGoalRed(int Owner, bool dunk);
	int OnGoalBlue(int Owner, bool dunk);
	virtual void StartRound();
	virtual void DoWincheck();
};
#endif
