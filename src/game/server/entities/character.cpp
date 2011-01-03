/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <new>
#include <stdio.h>
#include <string.h>
#include <engine/shared/config.h>
#include <game/server/gamecontext.h>
#include <game/server/score.h>
#include <game/mapitems.h>

#include "../gamemodes/race.h"
#include "../gamemodes/fastcap.h"

#include "character.h"
#include "flag.h"
#include "laser.h"
#include "projectile.h"

//input count
struct CInputCount
{
	int m_Presses;
	int m_Releases;
};

CInputCount CountInput(int Prev, int Cur)
{
	CInputCount c = {0, 0};
	Prev &= INPUT_STATE_MASK;
	Cur &= INPUT_STATE_MASK;
	int i = Prev;
	
	while(i != Cur)
	{
		i = (i+1)&INPUT_STATE_MASK;
		if(i&1)
			c.m_Presses++;
		else
			c.m_Releases++;
	}

	return c;
}


MACRO_ALLOC_POOL_ID_IMPL(CCharacter, MAX_CLIENTS)

// Character, "physical" player's part
CCharacter::CCharacter(CGameWorld *pWorld)
: CEntity(pWorld, NETOBJTYPE_CHARACTER)
{
	m_ProximityRadius = ms_PhysSize;
	m_Health = 0;
	m_Armor = 0;
}

void CCharacter::Reset()
{
	Destroy();
}

bool CCharacter::Spawn(CPlayer *pPlayer, vec2 Pos)
{
	m_PlayerState = PLAYERSTATE_UNKNOWN;
	m_EmoteStop = -1;
	m_LastAction = -1;
	m_ActiveWeapon = WEAPON_HAMMER;
	m_LastWeapon = WEAPON_HAMMER;
	m_QueuedWeapon = -1;
	
	m_RaceState = RACE_NONE;
	m_CpActive = -1;

	m_pFlag = 0;
	
	m_pPlayer = pPlayer;
	m_Pos = Pos;
	m_PrevPos = Pos;
	
	m_Core.Reset();
	m_Core.Init(&GameServer()->m_World.m_Core, GameServer()->Collision());
	m_Core.m_Pos = m_Pos;
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = &m_Core;

	m_ReckoningTick = 0;
	mem_zero(&m_SendCore, sizeof(m_SendCore));
	mem_zero(&m_ReckoningCore, sizeof(m_ReckoningCore));
	
	GameServer()->m_World.InsertEntity(this);
	m_Alive = true;

	m_LastSpeedup = -1;
	
	GameServer()->m_pController->OnCharacterSpawn(this);

	return true;
}

void CCharacter::Destroy()
{
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	m_Alive = false;
}

void CCharacter::SetWeapon(int W)
{
	if(W == m_ActiveWeapon)
		return;
		
	m_LastWeapon = m_ActiveWeapon;
	m_QueuedWeapon = -1;
	m_ActiveWeapon = W;
	GameServer()->CreateSound(m_Pos, SOUND_WEAPON_SWITCH, CmaskRace(GameServer(), m_pPlayer->GetCID()));
	
	if(m_ActiveWeapon < 0 || m_ActiveWeapon >= NUM_WEAPONS)
		m_ActiveWeapon = 0;
}

bool CCharacter::IsGrounded()
{
	if(GameServer()->Collision()->CheckPoint(m_Pos.x+m_ProximityRadius/2, m_Pos.y+m_ProximityRadius/2+5))
		return true;
	if(GameServer()->Collision()->CheckPoint(m_Pos.x-m_ProximityRadius/2, m_Pos.y+m_ProximityRadius/2+5))
		return true;
	return false;
}


void CCharacter::HandleNinja()
{
	if(m_ActiveWeapon != WEAPON_NINJA)
		return;
	
	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

	if ((Server()->Tick() - m_Ninja.m_ActivationTick) > (g_pData->m_Weapons.m_Ninja.m_Duration * Server()->TickSpeed() / 1000))
	{
		// time's up, return
		m_aWeapons[WEAPON_NINJA].m_Got = false;
		m_ActiveWeapon = m_LastWeapon;
		if(m_ActiveWeapon == WEAPON_NINJA)
			m_ActiveWeapon = WEAPON_GUN;
			
		SetWeapon(m_ActiveWeapon);
		return;
	}
	
	// force ninja Weapon
	SetWeapon(WEAPON_NINJA);

	m_Ninja.m_CurrentMoveTime--;

	if (m_Ninja.m_CurrentMoveTime == 0)
	{
		// reset velocity
		m_Core.m_Vel *= 0.2f;
	}

	if (m_Ninja.m_CurrentMoveTime > 0)
	{
		// Set velocity
		m_Core.m_Vel = m_Ninja.m_ActivationDir * g_pData->m_Weapons.m_Ninja.m_Velocity;
		vec2 OldPos = m_Pos;
		GameServer()->Collision()->MoveBox(&m_Core.m_Pos, &m_Core.m_Vel, vec2(m_ProximityRadius, m_ProximityRadius), 0.f);
		
		// reset velocity so the client doesn't predict stuff
		m_Core.m_Vel = vec2(0.f, 0.f);

		/*/ check if we Hit anything along the way
		{
			CCharacter *aEnts[64];
			vec2 Dir = m_Pos - OldPos;
			float Radius = m_ProximityRadius * 2.0f;
			vec2 Center = OldPos + Dir * 0.5f;
			int Num = -1;
			Num = GameServer()->m_World.FindEntities(Center, Radius, (CEntity**)aEnts, 64, NETOBJTYPE_CHARACTER);

			for (int i = 0; i < Num; ++i)
			{
				if (aEnts[i] == this)
					continue;
					
				// make sure we haven't Hit this object before
				bool bAlreadyHit = false;
				for (int j = 0; j < m_NumObjectsHit; j++)
				{
					if (m_apHitObjects[j] == aEnts[i])
						bAlreadyHit = true;
				}
				if (bAlreadyHit)
					continue;

				// check so we are sufficiently close
				if (distance(aEnts[i]->m_Pos, m_Pos) > (m_ProximityRadius * 2.0f))
					continue;

				// Hit a player, give him damage and stuffs...
				for(int j = 0; j < MAX_CLIENTS; j++)
				{
					if(GameServer()->m_apPlayers[j] && (GameServer()->m_apPlayers[j]->m_ShowOthers || j == m_pPlayer->GetCID()))
						GameServer()->CreateSound(m_Pos, SOUND_NINJA_HIT, CmaskOne(j));
				}
				// set his velocity to fast upward (for now)
				if(m_NumObjectsHit < 10)
					m_apHitObjects[m_NumObjectsHit++] = aEnts[i];
					
				aEnts[i]->TakeDamage(vec2(0, 10.0f), g_pData->m_Weapons.m_Ninja.m_pBase->m_Damage, m_pPlayer->GetCID(), WEAPON_NINJA);
			}
		}*/
		
		return;
	}

	return;
}


void CCharacter::DoWeaponSwitch()
{
	// make sure we can switch
	if(m_ReloadTimer != 0 || m_QueuedWeapon == -1 || m_aWeapons[WEAPON_NINJA].m_Got)
		return;

	// switch Weapon
	SetWeapon(m_QueuedWeapon);
}

void CCharacter::HandleWeaponSwitch()
{
	int WantedWeapon = m_ActiveWeapon;
	if(m_QueuedWeapon != -1)
		WantedWeapon = m_QueuedWeapon;
	
	// select Weapon
	int Next = CountInput(m_LatestPrevInput.m_NextWeapon, m_LatestInput.m_NextWeapon).m_Presses;
	int Prev = CountInput(m_LatestPrevInput.m_PrevWeapon, m_LatestInput.m_PrevWeapon).m_Presses;

	if(Next < 128) // make sure we only try sane stuff
	{
		while(Next) // Next Weapon selection
		{
			WantedWeapon = (WantedWeapon+1)%NUM_WEAPONS;
			if(m_aWeapons[WantedWeapon].m_Got)
				Next--;
		}
	}

	if(Prev < 128) // make sure we only try sane stuff
	{
		while(Prev) // Prev Weapon selection
		{
			WantedWeapon = (WantedWeapon-1)<0?NUM_WEAPONS-1:WantedWeapon-1;
			if(m_aWeapons[WantedWeapon].m_Got)
				Prev--;
		}
	}

	// Direct Weapon selection
	if(m_LatestInput.m_WantedWeapon)
		WantedWeapon = m_Input.m_WantedWeapon-1;

	// check for insane values
	if(WantedWeapon >= 0 && WantedWeapon < NUM_WEAPONS && WantedWeapon != m_ActiveWeapon && m_aWeapons[WantedWeapon].m_Got)
		m_QueuedWeapon = WantedWeapon;
	
	DoWeaponSwitch();
}

void CCharacter::FireWeapon()
{
	if(m_ReloadTimer != 0)
		return;
		
	DoWeaponSwitch();
	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));
	
	bool FullAuto = false;
	if(m_ActiveWeapon == WEAPON_GRENADE || m_ActiveWeapon == WEAPON_SHOTGUN || m_ActiveWeapon == WEAPON_RIFLE)
		FullAuto = true;


	// check if we gonna fire
	bool WillFire = false;
	if(CountInput(m_LatestPrevInput.m_Fire, m_LatestInput.m_Fire).m_Presses)
		WillFire = true;
		
	if(FullAuto && (m_LatestInput.m_Fire&1) && m_aWeapons[m_ActiveWeapon].m_Ammo)
		WillFire = true;
		
	if(!WillFire)
		return;
		
	// check for ammo
	if(!m_aWeapons[m_ActiveWeapon].m_Ammo)
	{
		// 125ms is a magical limit of how fast a human can click
		m_ReloadTimer = 125 * Server()->TickSpeed() / 1000;
		GameServer()->CreateSound(m_Pos, SOUND_WEAPON_NOAMMO, CmaskRace(GameServer(), m_pPlayer->GetCID()));
		return;
	}
	
	vec2 ProjStartPos = m_Pos+Direction*m_ProximityRadius*0.75f;
	
	switch(m_ActiveWeapon)
	{
		case WEAPON_HAMMER:
		{
			// reset objects Hit
			m_NumObjectsHit = 0;
			GameServer()->CreateSound(m_Pos, SOUND_HAMMER_FIRE, CmaskRace(GameServer(), m_pPlayer->GetCID()));
			
			/*CCharacter *aEnts[64];
			int Hits = 0;
			int Num = GameServer()->m_World.FindEntities(ProjStartPos, m_ProximityRadius*0.5f, (CEntity**)aEnts, 
			64, NETOBJTYPE_CHARACTER);

			for (int i = 0; i < Num; ++i)
			{
				CCharacter *Target = aEnts[i];
				
				//for race mod or any other mod, which needs hammer hits through the wall remove second condition
				if ((Target == this) || GameServer()->Collision()->IntersectLine(ProjStartPos, Target->m_Pos, NULL, NULL))
					continue;

				// set his velocity to fast upward (for now)
				GameServer()->CreateHammerHit(m_Pos);
				aEnts[i]->TakeDamage(vec2(0.f, -1.f), g_pData->m_Weapons.m_Hammer.m_pBase->m_Damage, m_pPlayer->GetCID(), m_ActiveWeapon);
				
				vec2 Dir;
				if (length(Target->m_Pos - m_Pos) > 0.0f)
					Dir = normalize(Target->m_Pos - m_Pos);
				else
					Dir = vec2(0.f, -1.f);
					
				Target->m_Core.m_Vel += normalize(Dir + vec2(0.f, -1.1f)) * 10.0f;
				Hits++;
			}
			
			// if we Hit anything, we have to wait for the reload
			if(Hits)
				m_ReloadTimer = Server()->TickSpeed()/3;*/
			
		} break;

		case WEAPON_GUN:
		{
			CProjectile *Proj = new CProjectile(GameWorld(), WEAPON_GUN,
				m_pPlayer->GetCID(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GunLifetime),
				1, 0, 0, -1, WEAPON_GUN);
				
			// pack the Projectile and send it to the client Directly
			CNetObj_Projectile p;
			Proj->FillInfo(&p);
			
			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(1);
			for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
				Msg.AddInt(((int *)&p)[i]);
				
			Server()->SendMsg(&Msg, 0, m_pPlayer->GetCID());
	
			GameServer()->CreateSound(m_Pos, SOUND_GUN_FIRE, CmaskRace(GameServer(), m_pPlayer->GetCID()));
		} break;
		
		case WEAPON_SHOTGUN:
		{
			int ShotSpread = 2;

			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(ShotSpread*2+1);
			
			for(int i = -ShotSpread; i <= ShotSpread; ++i)
			{
				float Spreading[] = {-0.185f, -0.070f, 0, 0.070f, 0.185f};
				float a = GetAngle(Direction);
				a += Spreading[i+2];
				float v = 1-(absolute(i)/(float)ShotSpread);
				float Speed = mix((float)GameServer()->Tuning()->m_ShotgunSpeeddiff, 1.0f, v);
				CProjectile *Proj = new CProjectile(GameWorld(), WEAPON_SHOTGUN,
					m_pPlayer->GetCID(),
					ProjStartPos,
					vec2(cosf(a), sinf(a))*Speed,
					(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_ShotgunLifetime),
					1, 0, 0, -1, WEAPON_SHOTGUN);
					
				// pack the Projectile and send it to the client Directly
				CNetObj_Projectile p;
				Proj->FillInfo(&p);
				
				for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
					Msg.AddInt(((int *)&p)[i]);
			}

			Server()->SendMsg(&Msg, 0,m_pPlayer->GetCID());					
			
			GameServer()->CreateSound(m_Pos, SOUND_SHOTGUN_FIRE, CmaskRace(GameServer(), m_pPlayer->GetCID()));
		} break;

		case WEAPON_GRENADE:
		{
			CProjectile *Proj = new CProjectile(GameWorld(), WEAPON_GRENADE,
				m_pPlayer->GetCID(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GrenadeLifetime),
				1, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);

			// pack the Projectile and send it to the client Directly
			CNetObj_Projectile p;
			Proj->FillInfo(&p);
			
			CMsgPacker Msg(NETMSGTYPE_SV_EXTRAPROJECTILE);
			Msg.AddInt(1);
			for(unsigned i = 0; i < sizeof(CNetObj_Projectile)/sizeof(int); i++)
				Msg.AddInt(((int *)&p)[i]);
			Server()->SendMsg(&Msg, 0, m_pPlayer->GetCID());
			
			GameServer()->CreateSound(m_Pos, SOUND_GRENADE_FIRE, CmaskRace(GameServer(), m_pPlayer->GetCID()));
		} break;
		
		case WEAPON_RIFLE:
		{
			new CLaser(GameWorld(), m_Pos, Direction, GameServer()->Tuning()->m_LaserReach, m_pPlayer->GetCID());
			GameServer()->CreateSound(m_Pos, SOUND_RIFLE_FIRE, CmaskRace(GameServer(), m_pPlayer->GetCID()));
		} break;
		
		case WEAPON_NINJA:
		{
			// reset Hit objects
			m_NumObjectsHit = 0;
			
			m_AttackTick = Server()->Tick();
			m_Ninja.m_ActivationDir = Direction;
			m_Ninja.m_CurrentMoveTime = g_pData->m_Weapons.m_Ninja.m_Movetime * Server()->TickSpeed() / 1000;

			GameServer()->CreateSound(m_Pos, SOUND_NINJA_FIRE, CmaskRace(GameServer(), m_pPlayer->GetCID()));
		} break;
		
	}
	
	m_AttackTick = Server()->Tick();
	
	if(m_aWeapons[m_ActiveWeapon].m_Ammo > 0 && !g_Config.m_SvInfiniteAmmo) // -1 == unlimited
		m_aWeapons[m_ActiveWeapon].m_Ammo--;
	
	if(!m_ReloadTimer)
		m_ReloadTimer = g_pData->m_Weapons.m_aId[m_ActiveWeapon].m_Firedelay * Server()->TickSpeed() / 1000;
}

void CCharacter::HandleWeapons()
{
	//ninja
	HandleNinja();
	
	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

	// check reload timer
	if(m_ReloadTimer)
	{
		m_ReloadTimer--;
		return;
	}

	// fire Weapon, if wanted
	FireWeapon();

	// ammo regen
	int AmmoRegenTime = g_pData->m_Weapons.m_aId[m_ActiveWeapon].m_Ammoregentime;
	if(AmmoRegenTime)
	{
		// If equipped and not active, regen ammo?
		if (m_ReloadTimer <= 0)
		{
			if (m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart < 0)
				m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = Server()->Tick();

			if ((Server()->Tick() - m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart) >= AmmoRegenTime * Server()->TickSpeed() / 1000)
			{
				// Add some ammo
				m_aWeapons[m_ActiveWeapon].m_Ammo = min(m_aWeapons[m_ActiveWeapon].m_Ammo + 1, 10);
				m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = -1;
			}
		}
		else
		{
			m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = -1;
		}
	}
	
	return;
}

bool CCharacter::GiveWeapon(int Weapon, int Ammo)
{
	if(m_aWeapons[Weapon].m_Ammo < g_pData->m_Weapons.m_aId[Weapon].m_Maxammo || !m_aWeapons[Weapon].m_Got)
	{	
		m_aWeapons[Weapon].m_Got = true;
		m_aWeapons[Weapon].m_Ammo = min(g_pData->m_Weapons.m_aId[Weapon].m_Maxammo, Ammo);
		return true;
	}
	return false;
}

void CCharacter::GiveNinja()
{
	m_Ninja.m_ActivationTick = Server()->Tick();
	m_aWeapons[WEAPON_NINJA].m_Got = true;
	m_aWeapons[WEAPON_NINJA].m_Ammo = -1;
	m_LastWeapon = m_ActiveWeapon;
	m_ActiveWeapon = WEAPON_NINJA;
	
	GameServer()->CreateSound(m_Pos, SOUND_PICKUP_NINJA, CmaskRace(GameServer(), m_pPlayer->GetCID()));
}

void CCharacter::SetEmote(int Emote, int Tick)
{
	m_EmoteType = Emote;
	m_EmoteStop = Tick;
}

void CCharacter::OnPredictedInput(CNetObj_PlayerInput *pNewInput)
{
	// check for changes
	if(mem_comp(&m_Input, pNewInput, sizeof(CNetObj_PlayerInput)) != 0)
		m_LastAction = Server()->Tick();
		
	// copy new input
	mem_copy(&m_Input, pNewInput, sizeof(m_Input));
	m_NumInputs++;
	
	// or are not allowed to aim in the center
	if(m_Input.m_TargetX == 0 && m_Input.m_TargetY == 0)
		m_Input.m_TargetY = -1;	
}

void CCharacter::OnDirectInput(CNetObj_PlayerInput *pNewInput)
{
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
	mem_copy(&m_LatestInput, pNewInput, sizeof(m_LatestInput));
	
	if(m_NumInputs > 2 && m_pPlayer->GetTeam() != TEAM_SPECTATORS)
	{
		HandleWeaponSwitch();
		FireWeapon();
	}
	
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
}

void CCharacter::Tick()
{
	if(m_pPlayer->m_ForceBalanced)
	{
		char Buf[128];
		str_format(Buf, sizeof(Buf), "You were moved to %s due to team balancing", GameServer()->m_pController->GetTeamName(m_pPlayer->GetTeam()));
		GameServer()->SendBroadcast(Buf, m_pPlayer->GetCID());
		
		m_pPlayer->m_ForceBalanced = false;
	}

	// save jumping state
	int Jumped = m_Core.m_Jumped;
	
	m_Core.m_Input = m_Input;
	m_Core.Tick(true);
	
	//race
	char aBuftime[128];
	float Time = (float)(Server()->Tick()-m_Starttime)/((float)Server()->TickSpeed());
	CPlayerData *pData = GameServer()->Score()->PlayerData(m_pPlayer->GetCID());
	
	
	// tile index
	int TileIndex = GameServer()->Collision()->GetIndex(m_PrevPos, m_Pos);
	
	int z = GameServer()->Collision()->IsCheckpoint(TileIndex);
	if(z != -1 && m_RaceState == RACE_STARTED)
	{
		m_CpActive = z;
		m_CpCurrent[z] = Time;
		m_CpTick = Server()->Tick() + Server()->TickSpeed()*2;
	}
	if(m_RaceState == RACE_STARTED && Server()->Tick()-m_Refreshtime >= Server()->TickSpeed())
	{
		int IntTime = (int)Time;
		
		if(m_pPlayer->m_IsUsingRaceClient)
		{
			CNetMsg_Sv_RaceTime Msg;
			Msg.m_Time = IntTime;
			Msg.m_Check = 0;
			
			if(m_CpActive != -1 && m_CpTick > Server()->Tick())
			{
				if(pData->m_BestTime && pData->m_aBestCpTime[m_CpActive] != 0)
				{
					float Diff = (m_CpCurrent[m_CpActive] - pData->m_aBestCpTime[m_CpActive])*100;
					Msg.m_Check = (int)Diff;
				}
			}
			
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, m_pPlayer->GetCID());
		}
		else
		{
			str_format(aBuftime, sizeof(aBuftime), "Current Time: %d min %d sec", IntTime/60, IntTime%60);
			
			if(m_CpActive != -1 && m_CpTick > Server()->Tick())
			{
				if(pData->m_BestTime && pData->m_aBestCpTime[m_CpActive] != 0)
				{
					char aTmp[128];
					float Diff = m_CpCurrent[m_CpActive] - pData->m_aBestCpTime[m_CpActive];
					str_format(aTmp, sizeof(aTmp), "\nCheckpoint | Diff : %+5.2f", Diff);
					strcat(aBuftime, aTmp);
				}
			}
			
			GameServer()->SendBroadcast(aBuftime, m_pPlayer->GetCID());
		}
		
		m_Refreshtime = Server()->Tick();
	}
	
	if(g_Config.m_SvRegen > 0 && (Server()->Tick()%g_Config.m_SvRegen) == 0)
	{
		if(m_Health < 10)
			m_Health++;
		else if(m_Armor < 10)
			m_Armor++;
	}
	
	if((GameServer()->Collision()->GetCollisionRace(TileIndex) == TILE_BEGIN && ((!m_aWeapons[WEAPON_GRENADE].m_Got && !m_Armor) || (m_RaceState != RACE_FINISHED && m_RaceState != RACE_STARTED)))
		|| (GameServer()->m_pController->IsFastCap() && m_RaceState != RACE_STARTED && ((CGameControllerFC*)GameServer()->m_pController)->IsEnemyFlagStand(m_Pos, m_pPlayer->GetTeam())))
	{
		// create flag
		if(GameServer()->m_pController->IsFastCap())
		{
			m_pFlag = new CFlag(GameWorld(), (m_pPlayer->GetTeam()+1)&1, m_Pos, this);
			
			// sound
			GameServer()->CreateSoundGlobal(SOUND_CTF_GRAB_EN, m_pPlayer->GetCID());
		}
		
		if(m_RaceState != RACE_NONE)
		{
			// reset pickups
			if(!m_aWeapons[WEAPON_GRENADE].m_Got)
				m_pPlayer->m_ResetPickups = true;
				
			// reset shield
			if(!GameServer()->m_pController->IsFastCap())
				m_Armor = 0;
		}
			
		m_Starttime = Server()->Tick();
		m_Refreshtime = Server()->Tick();
		m_RaceState = RACE_STARTED;
	}
	else if((GameServer()->Collision()->GetCollisionRace(TileIndex) == TILE_END || (GameServer()->m_pController->IsFastCap() && ((CGameControllerFC*)GameServer()->m_pController)->IsOwnFlagStand(m_Pos, m_pPlayer->GetTeam()))) && m_RaceState == RACE_STARTED)
	{
		// reset the flag
		if(GameServer()->m_pController->IsFastCap() && m_pFlag)
		{
			m_pFlag->Reset();
			m_pFlag = 0;
			
			// reset pickups
			m_pPlayer->m_ResetPickups = true;
			
			// sound \o/
			GameServer()->CreateSoundGlobal(SOUND_CTF_CAPTURE, m_pPlayer->GetCID());
		}
		
        // calculate finish time
        float FinishTime = CalculateFinishTime(Time, m_PrevPos, m_Pos);
        
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "%s finished in: %d minute(s) %6.3f second(s)", Server()->ClientName(m_pPlayer->GetCID()), (int)FinishTime/60, FinishTime-((int)FinishTime/60*60));
		if(!g_Config.m_SvShowTimes)
			GameServer()->SendChatTarget(m_pPlayer->GetCID(), aBuf);
		else
			GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
		
		if(FinishTime - pData->m_BestTime < 0)
		{
			// new record \o/
			str_format(aBuf, sizeof(aBuf), "New record: %6.3f second(s) better", FinishTime - pData->m_BestTime);
			if(!g_Config.m_SvShowTimes)
				GameServer()->SendChatTarget(m_pPlayer->GetCID(), aBuf);
			else
				GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
		}
		
		if(!pData->m_BestTime || FinishTime < pData->m_BestTime)
		{
			// update the score
			pData->Set(FinishTime, m_CpCurrent);
			
			if(str_comp_num(Server()->ClientName(m_pPlayer->GetCID()), "nameless tee", 12) != 0)
				GameServer()->Score()->SaveScore(m_pPlayer->GetCID(), FinishTime, this);
		}
		
		// update server best time
		if(!GameServer()->m_pController->m_CurrentRecord || FinishTime < GameServer()->m_pController->m_CurrentRecord)
		{
			// check for nameless
			if(str_comp_num(Server()->ClientName(m_pPlayer->GetCID()), "nameless tee", 12) != 0)
				GameServer()->m_pController->m_CurrentRecord = FinishTime;
		}
		
		m_RaceState = RACE_FINISHED;

		// set player score
		if(!GameServer()->Score()->PlayerData(m_pPlayer->GetCID())->m_CurrentTime || GameServer()->Score()->PlayerData(m_pPlayer->GetCID())->m_CurrentTime > FinishTime)
		{
			GameServer()->Score()->PlayerData(m_pPlayer->GetCID())->m_CurrentTime = FinishTime;
			
			// send it to all players
			/*for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->m_IsUsingRaceClient)
				{
					if(g_Config.m_SvShowTimes || i == m_pPlayer->GetCID())
					{
						CNetMsg_Sv_PlayerTime Msg;
						char aBuf[16];
						str_format(aBuf, sizeof(aBuf), "%.0f", FinishTime*100.0f); // damn ugly but the only way i know to do it
						int TimeToSend;
						sscanf(aBuf, "%d", &TimeToSend);
						Msg.m_Time = TimeToSend;
						Msg.m_Cid = m_pPlayer->GetCID();
						Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, i);
					}
				}
			}*/
		}
		
		int TTime = 0-(int)FinishTime;
		if(m_pPlayer->m_Score < TTime)
			m_pPlayer->m_Score = TTime;
	}
	else if(TileIndex != -1 && GameServer()->Collision()->GetCollisionRace(TileIndex) == TILE_STOPL)
	{
		if(m_Core.m_Vel.x > 0)
		{
			if((int)GameServer()->Collision()->GetPos(TileIndex).x < (int)m_Core.m_Pos.x)
				m_Core.m_Pos.x = m_PrevPos.x;
			m_Core.m_Vel.x = 0;
		}
	}
	else if(TileIndex != -1 && GameServer()->Collision()->GetCollisionRace(TileIndex) == TILE_STOPR)
	{
		if(m_Core.m_Vel.x < 0)
		{
			if((int)GameServer()->Collision()->GetPos(TileIndex).x > (int)m_Core.m_Pos.x)
				m_Core.m_Pos.x = m_PrevPos.x;
			m_Core.m_Vel.x = 0;
		}
	}
	else if(TileIndex != -1 && GameServer()->Collision()->GetCollisionRace(TileIndex) == TILE_STOPB)
	{
		if(m_Core.m_Vel.y < 0)
		{
			if((int)GameServer()->Collision()->GetPos(TileIndex).y > (int)m_Core.m_Pos.y)
				m_Core.m_Pos.y = m_PrevPos.y;
			m_Core.m_Vel.y = 0;
		}
	}
	else if(TileIndex != -1 && GameServer()->Collision()->GetCollisionRace(TileIndex) == TILE_STOPT)
	{
		if(m_Core.m_Vel.y > 0)
		{
			if((int)GameServer()->Collision()->GetPos(TileIndex).y < (int)m_Core.m_Pos.y)
				m_Core.m_Pos.y = m_PrevPos.y;
			if(Jumped&3 && m_Core.m_Jumped != Jumped) // check double jump
				m_Core.m_Jumped = Jumped;
			m_Core.m_Vel.y = 0;
		}
	}
	
	// handle speedup tiles
	int CurrentSpeedup = GameServer()->Collision()->IsSpeedup(TileIndex);
	bool SpeedupTouch = false;
	if(m_LastSpeedup != CurrentSpeedup && CurrentSpeedup > -1)
	{
		vec2 Direction;
		int Force;
		GameServer()->Collision()->GetSpeedup(TileIndex, &Direction, &Force);
		
		m_Core.m_Vel += Direction*Force;
		
		SpeedupTouch = true;
	}
	
	m_LastSpeedup = CurrentSpeedup;
	
	// handle teleporter
	z = GameServer()->Collision()->IsTeleport(TileIndex);
	if(g_Config.m_SvTeleport && z)
	{
		// check double jump
		if(Jumped&3 && m_Core.m_Jumped != Jumped)
			m_Core.m_Jumped = Jumped;
				
		m_Core.m_HookedPlayer = -1;
		m_Core.m_HookState = HOOK_RETRACTED;
		m_Core.m_TriggeredEvents |= COREEVENT_HOOK_RETRACT;
		m_Core.m_HookState = HOOK_RETRACTED;
		m_Core.m_Pos = ((CGameControllerRACE*)GameServer()->m_pController)->m_pTeleporter[z-1];
		m_Core.m_HookPos = m_Core.m_Pos;
		//Resetting velocity to prevent exploit
		if(g_Config.m_SvTeleportVelReset)
			m_Core.m_Vel = vec2(0,0);
		if(g_Config.m_SvStrip)
		{
			m_ActiveWeapon = WEAPON_HAMMER;
			m_LastWeapon = WEAPON_HAMMER;
			m_aWeapons[0].m_Got = true;
			for(int i = 1; i < NUM_WEAPONS; i++)
			m_aWeapons[i].m_Got = false;
		}
	}
	
	// set Position just in case it was changed
	m_Pos = m_Core.m_Pos;
	
	// handle death-tiles
	if(!SpeedupTouch &&
		(GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/3.f, m_Pos.y-m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/3.f, m_Pos.y+m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/3.f, m_Pos.y-m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/3.f, m_Pos.y+m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH))
	{
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
	}

	// kill player when leaving gamelayer
	if((int)m_Pos.x/32 < -200 || (int)m_Pos.x/32 > GameServer()->Collision()->GetWidth()+200 ||
		(int)m_Pos.y/32 < -200 || (int)m_Pos.y/32 > GameServer()->Collision()->GetHeight()+200)
	{
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
	}
	
	// handle Weapons
	HandleWeapons();

	m_PlayerState = m_Input.m_PlayerState;

	// Previnput
	m_PrevInput = m_Input;
	
	m_PrevPos = m_Core.m_Pos;
	return;
}

void CCharacter::TickDefered()
{
	// advance the dummy
	{
		CWorldCore TempWorld;
		m_ReckoningCore.Init(&TempWorld, GameServer()->Collision());
		m_ReckoningCore.Tick(false);
		m_ReckoningCore.Move();
		m_ReckoningCore.Quantize();
	}
	
	//lastsentcore
	vec2 StartPos = m_Core.m_Pos;
	vec2 StartVel = m_Core.m_Vel;
	bool StuckBefore = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	
	m_Core.Move();
	bool StuckAfterMove = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	m_Core.Quantize();
	bool StuckAfterQuant = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	m_Pos = m_Core.m_Pos;
	
	if(!StuckBefore && (StuckAfterMove || StuckAfterQuant))
	{
		// Hackish solution to get rid of strict-aliasing warning
		union
		{
			float f;
			unsigned u;
		}StartPosX, StartPosY, StartVelX, StartVelY;

		StartPosX.f = StartPos.x;
		StartPosY.f = StartPos.y;
		StartVelX.f = StartVel.x;
		StartVelY.f = StartVel.y;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "STUCK!!! %d %d %d %f %f %f %f %x %x %x %x", 
			StuckBefore,
			StuckAfterMove,
			StuckAfterQuant,
			StartPos.x, StartPos.y,
			StartVel.x, StartVel.y,
			StartPosX.u, StartPosY.u,
			StartVelX.u, StartVelY.u);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	int Events = m_Core.m_TriggeredEvents;
	int Mask = ~(CmaskRace(GameServer(), m_pPlayer->GetCID())^CmaskAllExceptOne(m_pPlayer->GetCID()));
	
	if(Events&COREEVENT_GROUND_JUMP) GameServer()->CreateSound(m_Pos, SOUND_PLAYER_JUMP, Mask);
	
	if(Events&COREEVENT_HOOK_ATTACH_PLAYER) GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_PLAYER, CmaskRace(GameServer(), m_pPlayer->GetCID()));
	if(Events&COREEVENT_HOOK_ATTACH_GROUND)	GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_GROUND, Mask);
	if(Events&COREEVENT_HOOK_HIT_NOHOOK) GameServer()->CreateSound(m_Pos, SOUND_HOOK_NOATTACH, Mask);
	
	if(m_pPlayer->GetTeam() == TEAM_SPECTATORS)
	{
		m_Pos.x = m_Input.m_TargetX;
		m_Pos.y = m_Input.m_TargetY;
	}
	
	// update the m_SendCore if needed
	{
		CNetObj_Character Predicted;
		CNetObj_Character Current;
		mem_zero(&Predicted, sizeof(Predicted));
		mem_zero(&Current, sizeof(Current));
		m_ReckoningCore.Write(&Predicted);
		m_Core.Write(&Current);

		// only allow dead reackoning for a top of 3 seconds
		if(m_ReckoningTick+Server()->TickSpeed()*3 < Server()->Tick() || mem_comp(&Predicted, &Current, sizeof(CNetObj_Character)) != 0)
		{
			m_ReckoningTick = Server()->Tick();
			m_SendCore = m_Core;
			m_ReckoningCore = m_Core;
		}
	}
}

float CCharacter::CalculateFinishTime(float Time, vec2 PrevPos, vec2 Pos)
{
	for(int i = 0; i <= 20; i++)
	{
		float a = i/20.0f;
		vec2 TmpPos = mix(PrevPos, Pos, a);
		if(GameServer()->Collision()->GetCollisionRace(GameServer()->Collision()->GetIndex(TmpPos)) == TILE_END || 
			(GameServer()->m_pController->IsFastCap() && ((CGameControllerFC*)GameServer()->m_pController)->IsOwnFlagStand(TmpPos, m_pPlayer->GetTeam())))
			return Time + (float)i/1000.f;
	}
	
	return Time;
}

bool CCharacter::IncreaseHealth(int Amount)
{
	if(m_Health >= 10)
		return false;
	m_Health = clamp(m_Health+Amount, 0, 10);
	return true;
}

bool CCharacter::IncreaseArmor(int Amount)
{
	if(m_Armor >= 10)
		return false;
	m_Armor = clamp(m_Armor+Amount, 0, 10);
	return true;
}

void CCharacter::Die(int Killer, int Weapon)
{
	int ModeSpecial = GameServer()->m_pController->OnCharacterDeath(this, GameServer()->m_apPlayers[Killer], Weapon);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "kill killer='%d:%s' victim='%d:%s' weapon=%d special=%d",
		Killer, Server()->ClientName(Killer),
		m_pPlayer->GetCID(), Server()->ClientName(m_pPlayer->GetCID()), Weapon, ModeSpecial);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// send the kill message
	CNetMsg_Sv_KillMsg Msg;
	Msg.m_Killer = Killer;
	Msg.m_Victim = m_pPlayer->GetCID();
	Msg.m_Weapon = Weapon;
	Msg.m_ModeSpecial = ModeSpecial;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, -1);

	// a nice sound
	GameServer()->CreateSound(m_Pos, SOUND_PLAYER_DIE, CmaskRace(GameServer(), m_pPlayer->GetCID()));
	
	// this is for auto respawn after 3 secs
	m_pPlayer->m_DieTick = Server()->Tick();
	
	m_Alive = false;
	GameServer()->m_World.RemoveEntity(this);
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCID());
	
	// we got to wait 0.5 secs before respawning
	m_pPlayer->m_RespawnTick = Server()->Tick()+Server()->TickSpeed()/2;
	
	// reset pickups
	m_pPlayer->m_ResetPickups = true;
	
	// reset flag
	if(m_pFlag)
		m_pFlag->Reset();
}

bool CCharacter::TakeDamage(vec2 Force, int Dmg, int From, int Weapon)
{
	if(From == m_pPlayer->GetCID())
		m_Core.m_Vel += Force;
	
	if(GameServer()->m_pController->IsFriendlyFire(m_pPlayer->GetCID(), From) && !g_Config.m_SvTeamdamage)
		return false;

	// player only inflicts half damage on self
	if(From == m_pPlayer->GetCID())
		Dmg = max(1, Dmg/2);

	if(((From == m_pPlayer->GetCID() && !g_Config.m_SvRocketJumpDamage) || From != m_pPlayer->GetCID()))
		Dmg = 0;

	m_DamageTaken++;

	// create healthmod indicator
	if(Server()->Tick() < m_DamageTakenTick+25)
	{
		// make sure that the damage indicators doesn't group together
		GameServer()->CreateDamageInd(m_Pos, m_DamageTaken*0.25f, Dmg, m_pPlayer->GetCID());
	}
	else
	{
		m_DamageTaken = 0;
		GameServer()->CreateDamageInd(m_Pos, 0, Dmg, m_pPlayer->GetCID());
	}

	if(Dmg)
	{
		if(m_Armor)
		{
			if(Dmg > 1)
			{
				m_Health--;
				Dmg--;
			}
			
			if(Dmg > m_Armor)
			{
				Dmg -= m_Armor;
				m_Armor = 0;
			}
			else
			{
				m_Armor -= Dmg;
				Dmg = 0;
			}
		}
		
		m_Health -= Dmg;
	}

	m_DamageTakenTick = Server()->Tick();

	// do damage Hit sound
	/*if(From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
		GameServer()->CreateSound(GameServer()->m_apPlayers[From]->m_ViewPos, SOUND_HIT, CmaskOne(From));*/

	// check for death
	if(m_Health <= 0)
	{
		Die(From, Weapon);
		
		// set attacker's face to happy (taunt!)
		if (From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
		{
			CCharacter *pChr = GameServer()->m_apPlayers[From]->GetCharacter();
			if (pChr)
			{
				pChr->m_EmoteType = EMOTE_HAPPY;
				pChr->m_EmoteStop = Server()->Tick() + Server()->TickSpeed();
			}
		}
	
		return false;
	}

	if (Dmg > 2)
		GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_LONG, CmaskRace(GameServer(), m_pPlayer->GetCID()));
	else
		GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_SHORT, CmaskRace(GameServer(), m_pPlayer->GetCID()));

	// dmg emote only for self dmg
	if(From == m_pPlayer->GetCID())
	{
		m_EmoteType = EMOTE_PAIN;
		m_EmoteStop = Server()->Tick() + 500 * Server()->TickSpeed() / 1000;
	}

	return true;
}

void CCharacter::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient) || (!GameServer()->m_apPlayers[SnappingClient]->m_ShowOthers && SnappingClient != m_pPlayer->GetCID()))
		return;
	
	CNetObj_Character *pCharacter = static_cast<CNetObj_Character *>(Server()->SnapNewItem(NETOBJTYPE_CHARACTER, m_pPlayer->GetCID(), sizeof(CNetObj_Character)));
	if(!pCharacter)
		return;
	
	// write down the m_Core
	if(!m_ReckoningTick || GameServer()->m_World.m_Paused)
	{
		// no dead reckoning when paused because the client doesn't know
		// how far to perform the reckoning
		pCharacter->m_Tick = 0;
		m_Core.Write(pCharacter);
	}
	else
	{
		pCharacter->m_Tick = m_ReckoningTick;
		m_SendCore.Write(pCharacter);
	}
	
	// set emote
	if (m_EmoteStop < Server()->Tick())
	{
		m_EmoteType = EMOTE_NORMAL;
		m_EmoteStop = -1;
	}

	pCharacter->m_Emote = m_EmoteType;

	pCharacter->m_AmmoCount = 0;
	pCharacter->m_Health = 0;
	pCharacter->m_Armor = 0;
	
	pCharacter->m_Weapon = m_ActiveWeapon;
	pCharacter->m_AttackTick = m_AttackTick;

	pCharacter->m_Direction = m_Input.m_Direction;

	if(m_pPlayer->GetCID() == SnappingClient)
	{
		pCharacter->m_Health = m_Health;
		pCharacter->m_Armor = m_Armor;
		if(m_aWeapons[m_ActiveWeapon].m_Ammo > 0)
			pCharacter->m_AmmoCount = m_aWeapons[m_ActiveWeapon].m_Ammo;
	}

	if(pCharacter->m_Emote == EMOTE_NORMAL)
	{
		if(250 - ((Server()->Tick() - m_LastAction)%(250)) < 5)
			pCharacter->m_Emote = EMOTE_BLINK;
	}

	pCharacter->m_PlayerState = m_PlayerState;
}
