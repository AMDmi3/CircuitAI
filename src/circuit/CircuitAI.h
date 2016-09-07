/*
 * Circuit.h
 *
 *  Created on: Aug 9, 2014
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_CIRCUIT_H_
#define SRC_CIRCUIT_CIRCUIT_H_

#include "unit/AllyTeam.h"
#include "unit/CircuitDef.h"
#include "util/Defines.h"

#include <memory>
#include <unordered_map>
#include <map>
#include <vector>
#include <set>

namespace springai {
	class OOAICallback;
	class Log;
	class Game;
	class Map;
	class Pathing;
	class Drawer;
	class SkirmishAI;
}
struct SSkirmishAICallback;

namespace circuit {

#define ERROR_UNKNOWN			200
#define ERROR_INIT				(ERROR_UNKNOWN + EVENT_INIT)
#define ERROR_RELEASE			(ERROR_UNKNOWN + EVENT_RELEASE)
#define ERROR_UPDATE			(ERROR_UNKNOWN + EVENT_UPDATE)
#define ERROR_UNIT_CREATED		(ERROR_UNKNOWN + EVENT_UNIT_CREATED)
#define ERROR_UNIT_FINISHED		(ERROR_UNKNOWN + EVENT_UNIT_FINISHED)
#define ERROR_UNIT_IDLE			(ERROR_UNKNOWN + EVENT_UNIT_IDLE)
#define ERROR_UNIT_MOVE_FAILED	(ERROR_UNKNOWN + EVENT_UNIT_MOVE_FAILED)
#define ERROR_UNIT_DAMAGED		(ERROR_UNKNOWN + EVENT_UNIT_DAMAGED)
#define ERROR_UNIT_DESTROYED	(ERROR_UNKNOWN + EVENT_UNIT_DESTROYED)
#define ERROR_UNIT_GIVEN		(ERROR_UNKNOWN + EVENT_UNIT_GIVEN)
#define ERROR_UNIT_CAPTURED		(ERROR_UNKNOWN + EVENT_UNIT_CAPTURED)
#define ERROR_ENEMY_ENTER_LOS	(ERROR_UNKNOWN + EVENT_ENEMY_ENTER_LOS)
#define ERROR_ENEMY_LEAVE_LOS	(ERROR_UNKNOWN + EVENT_ENEMY_LEAVE_LOS)
#define ERROR_ENEMY_ENTER_RADAR	(ERROR_UNKNOWN + EVENT_ENEMY_ENTER_RADAR)
#define ERROR_ENEMY_LEAVE_RADAR	(ERROR_UNKNOWN + EVENT_ENEMY_LEAVE_RADAR)
#define ERROR_ENEMY_DAMAGED		(ERROR_UNKNOWN + EVENT_ENEMY_DAMAGED)
#define ERROR_ENEMY_DESTROYED	(ERROR_UNKNOWN + EVENT_ENEMY_DESTROYED)
#define LOG(fmt, ...)	GetLog()->DoLog(utils::string_format(std::string(fmt), ##__VA_ARGS__).c_str())

class CGameAttribute;
class CSetupManager;
class CThreatMap;
class CPathFinder;
class CTerrainManager;
class CBuilderManager;
class CFactoryManager;
class CEconomyManager;
class CMilitaryManager;
class CScheduler;
class IModule;
class CEnemyUnit;
#ifdef DEBUG_VIS
class CDebugDrawer;
#endif

class CCircuitAI {
public:
	enum class Difficulty: char {EASY, NORMAL, HARD};

public:
	CCircuitAI(springai::OOAICallback* callback);
	virtual ~CCircuitAI();

// ---- AI Event handler ---- BEGIN
public:
	int HandleEvent(int topic, const void* data);
	void NotifyGameEnd();
	void Resign() { isResigned = true; }
private:
	typedef int (CCircuitAI::*EventHandlerPtr)(int topic, const void* data);
	int HandleGameEvent(int topic, const void* data);
	int HandleEndEvent(int topic, const void* data);
	EventHandlerPtr eventHandler;
// ---- AI Event handler ---- END

private:
	bool IsModValid();
	int Init(int skirmishAIId, const struct SSkirmishAICallback* sAICallback);
	int Release(int reason);
	int Update(int frame);
	int Message(int playerId, const char* message);
	int UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder);
	int UnitFinished(CCircuitUnit* unit);
	int UnitIdle(CCircuitUnit* unit);
	int UnitMoveFailed(CCircuitUnit* unit);
	int UnitDamaged(CCircuitUnit* unit, CEnemyUnit* attacker/*, int weaponId*/);
	int UnitDestroyed(CCircuitUnit* unit, CEnemyUnit* attacker);
	int UnitGiven(CCircuitUnit::Id unitId, int oldTeamId, int newTeamId);
	int UnitCaptured(CCircuitUnit::Id unitId, int oldTeamId, int newTeamId);
	int EnemyEnterLOS(CEnemyUnit* enemy);
	int EnemyLeaveLOS(CEnemyUnit* enemy);
	int EnemyEnterRadar(CEnemyUnit* enemy);
	int EnemyLeaveRadar(CEnemyUnit* enemy);
	int EnemyDamaged(CEnemyUnit* enemy);
	int EnemyDestroyed(CEnemyUnit* enemy);
	int PlayerCommand(std::vector<CCircuitUnit*>& units);
//	int CommandFinished(CCircuitUnit* unit, int commandTopicId, springai::Command* cmd);
	int LuaMessage(const char* inData);

// ---- Units ---- BEGIN
private:
	CCircuitUnit* RegisterTeamUnit(CCircuitUnit::Id unitId);
	void UnregisterTeamUnit(CCircuitUnit* unit);
	void DeleteTeamUnit(CCircuitUnit* unit);
public:
	void Garbage(CCircuitUnit* unit, const char* message);
	CCircuitUnit* GetTeamUnit(CCircuitUnit::Id unitId) const;
	const CAllyTeam::Units& GetTeamUnits() const { return teamUnits; }

	void UpdateFriendlyUnits() { allyTeam->UpdateFriendlyUnits(this); }
	CCircuitUnit* GetFriendlyUnit(springai::Unit* u) const;
	CCircuitUnit* GetFriendlyUnit(CCircuitUnit::Id unitId) const { return allyTeam->GetFriendlyUnit(unitId); }
	const CAllyTeam::Units& GetFriendlyUnits() const { return allyTeam->GetFriendlyUnits(); }

	using EnemyUnits = std::map<CCircuitUnit::Id, CEnemyUnit*>;
private:
	CEnemyUnit* RegisterEnemyUnit(CCircuitUnit::Id unitId, bool isInLOS = false);
	void UnregisterEnemyUnit(CEnemyUnit* unit);
	void UpdateEnemyUnits();
public:
	CEnemyUnit* GetEnemyUnit(springai::Unit* u) const { return GetEnemyUnit(u->GetUnitId()); }
	CEnemyUnit* GetEnemyUnit(CCircuitUnit::Id unitId) const;
	const EnemyUnits& GetEnemyUnits() const { return enemyUnits; }

	CAllyTeam* GetAllyTeam() const { return allyTeam; }

	void AddActionUnit(CCircuitUnit* unit) { actionUnits.push_back(unit); }

private:
	void ActionUpdate();

	CAllyTeam::Units teamUnits;  // owner
	EnemyUnits enemyUnits;  // owner
	CAllyTeam* allyTeam;
	int uEnemyMark;
	int kEnemyMark;

	std::vector<CCircuitUnit*> actionUnits;
	unsigned int actionIterator;

	std::set<CCircuitUnit*> garbage;
// ---- Units ---- END

// ---- AIOptions.lua ---- BEGIN
public:
	Difficulty GetDifficulty() const { return difficulty; }
	bool IsAllyAware() const { return allyAware; }
private:
	std::string InitOptions();
	Difficulty difficulty;
	bool allyAware;
// ---- AIOptions.lua ---- END

// ---- UnitDefs ---- BEGIN
public:
	using CircuitDefs = std::unordered_map<CCircuitDef::Id, CCircuitDef*>;
	using NamedDefs = std::map<const char*, CCircuitDef*, cmp_str>;

	const CircuitDefs& GetCircuitDefs() const { return defsById; }
	CCircuitDef* GetCircuitDef(const char* name);
	CCircuitDef* GetCircuitDef(CCircuitDef::Id unitDefId);
private:
	void InitUnitDefs();
	CircuitDefs defsById;  // owner
	NamedDefs defsByName;
// ---- UnitDefs ---- END

public:
	bool IsInitialized() const { return isInitialized; }
	CGameAttribute* GetGameAttribute() const { return gameAttribute.get(); }
	std::shared_ptr<CScheduler>& GetScheduler() { return scheduler; }
	int GetLastFrame()    const { return lastFrame; }
	int GetSkirmishAIId() const { return skirmishAIId; }
	int GetTeamId()       const { return teamId; }
	int GetAllyTeamId()   const { return allyTeamId; }
	springai::OOAICallback* GetCallback()   const { return callback; }
	springai::Log*          GetLog()        const { return log.get(); }
	springai::Game*         GetGame()       const { return game.get(); }
	springai::Map*          GetMap()        const { return map.get(); }
	springai::Pathing*      GetPathing()    const { return pathing.get(); }
	springai::Drawer*       GetDrawer()     const { return drawer.get(); }
	springai::SkirmishAI*   GetSkirmishAI() const { return skirmishAI.get(); }
	springai::Team*         GetTeam()       const { return team.get(); }
	CSetupManager*    GetSetupManager()    const { return setupManager.get(); }
	CMetalManager*    GetMetalManager()    const { return metalManager.get(); }
	CThreatMap*       GetThreatMap()       const { return threatMap.get(); }
	CPathFinder*      GetPathfinder()      const { return pathfinder.get(); }
	CTerrainManager*  GetTerrainManager()  const { return terrainManager.get(); }
	CBuilderManager*  GetBuilderManager()  const { return builderManager.get(); }
	CFactoryManager*  GetFactoryManager()  const { return factoryManager.get(); }
	CEconomyManager*  GetEconomyManager()  const { return economyManager.get(); }
	CMilitaryManager* GetMilitaryManager() const { return militaryManager.get(); }

	int GetAirCategory()   const { return airCategory; }
	int GetLandCategory()  const { return landCategory; }
	int GetWaterCategory() const { return waterCategory; }
	int GetBadCategory()   const { return badCategory; }
	int GetGoodCategory()  const { return goodCategory; }

private:
	// debug
//	void DrawClusters();

	bool isInitialized;
	bool isResigned;
	int lastFrame;
	int skirmishAIId;
	int teamId;
	int allyTeamId;
	const struct SSkirmishAICallback* sAICallback;
	springai::OOAICallback*               callback;
	std::unique_ptr<springai::Log>        log;
	std::unique_ptr<springai::Game>       game;
	std::unique_ptr<springai::Map>        map;
	std::unique_ptr<springai::Pathing>    pathing;
	std::unique_ptr<springai::Drawer>     drawer;
	std::unique_ptr<springai::SkirmishAI> skirmishAI;
	std::unique_ptr<springai::Team>       team;

	static std::unique_ptr<CGameAttribute> gameAttribute;
	static unsigned int gaCounter;
	void CreateGameAttribute();
	void DestroyGameAttribute();
	std::shared_ptr<CScheduler> scheduler;
	std::shared_ptr<CSetupManager> setupManager;
	std::shared_ptr<CMetalManager> metalManager;
	std::shared_ptr<CThreatMap> threatMap;
	std::shared_ptr<CPathFinder> pathfinder;
	std::shared_ptr<CTerrainManager> terrainManager;
	std::shared_ptr<CBuilderManager> builderManager;
	std::shared_ptr<CFactoryManager> factoryManager;
	std::shared_ptr<CEconomyManager> economyManager;
	std::shared_ptr<CMilitaryManager> militaryManager;
	std::vector<std::shared_ptr<IModule>> modules;

	// TODO: Move into GameAttribute? Or use locally
	int airCategory;  // over surface
	int landCategory;  // on surface
	int waterCategory;  // under surface
	int badCategory;
	int goodCategory;

#ifdef DEBUG_VIS
private:
	std::shared_ptr<CDebugDrawer> debugDrawer;
public:
	std::shared_ptr<CDebugDrawer>& GetDebugDrawer() { return debugDrawer; }
#endif
};

} // namespace circuit

#endif // SRC_CIRCUIT_CIRCUIT_H_
