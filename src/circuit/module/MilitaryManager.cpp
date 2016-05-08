/*
 * MilitaryManager.cpp
 *
 *  Created on: Sep 5, 2014
 *      Author: rlcevg
 */

#include "module/MilitaryManager.h"
#include "module/BuilderManager.h"
#include "module/EconomyManager.h"
#include "resource/MetalManager.h"
#include "setup/SetupManager.h"
#include "setup/DefenceMatrix.h"
#include "task/NullTask.h"
#include "task/IdleTask.h"
#include "task/RetreatTask.h"
#include "task/builder/DefenceTask.h"
#include "task/fighter/RallyTask.h"
#include "task/fighter/GuardTask.h"
#include "task/fighter/DefendTask.h"
#include "task/fighter/ScoutTask.h"
#include "task/fighter/AttackTask.h"
#include "task/fighter/BombTask.h"
#include "task/fighter/MeleeTask.h"
#include "task/fighter/ArtilleryTask.h"
#include "terrain/TerrainManager.h"
#include "terrain/ThreatMap.h"
#include "terrain/PathFinder.h"
#include "CircuitAI.h"
#include "util/Scheduler.h"
#include "util/utils.h"
#include "json/json.h"

#include "OOAICallback.h"
#include "AISCommands.h"
#include "Command.h"
#include "WeaponDef.h"

namespace circuit {

using namespace springai;

CMilitaryManager::CMilitaryManager(CCircuitAI* circuit)
		: IUnitModule(circuit)
		, fightUpdateSlice(0)
		, retUpdateSlice(0)
		, scoutIdx(0)
		, metalArmy(.0f)
{
	CScheduler* scheduler = circuit->GetScheduler().get();
	scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::Watchdog, this),
							FRAMES_PER_SEC * 60,
							circuit->GetSkirmishAIId() * WATCHDOG_COUNT + 12);
	scheduler->RunTaskAt(std::make_shared<CGameTask>(&CMilitaryManager::Init, this));

	CCircuitDef::Id unitDefId;
	/*
	 * Defence handlers
	 */
	auto defenceFinishedHandler = [this](CCircuitUnit* unit) {
		unit->GetUnit()->Stockpile(UNIT_COMMAND_OPTION_SHIFT_KEY | UNIT_COMMAND_OPTION_CONTROL_KEY);
	};
	auto defenceDestroyedHandler = [this](CCircuitUnit* unit, CEnemyUnit* attacker) {
		int frame = this->circuit->GetLastFrame();
		float defCost = unit->GetCircuitDef()->GetCost();
		CDefenceMatrix::SDefPoint* point = defence->GetDefPoint(unit->GetPos(frame), defCost);
		if (point != nullptr) {
			point->cost -= defCost;
		}
	};
	const char* defenders[] = {"corllt", "corrl", "armdeva", "corhlt", "turrettorp", "corrazor", "armnanotc", "cordoom", "corjamt"/*, "armartic", "corjamt", "armanni", "corbhmth"*/};
	for (const char* name : defenders) {
		unitDefId = circuit->GetCircuitDef(name)->GetId();
		destroyedHandler[unitDefId] = defenceDestroyedHandler;
	}

	/*
	 * Attacker handlers
	 */
	auto attackerCreatedHandler = [this](CCircuitUnit* unit, CCircuitUnit* builder) {
		if (unit->GetTask() == nullptr) {
			unit->SetManager(this);
			nullTask->AssignTo(unit);
		}
	};
	auto attackerFinishedHandler = [this](CCircuitUnit* unit) {
		if (unit->GetTask() == nullptr) {
			unit->SetManager(this);
			idleTask->AssignTo(unit);
		} else {
			nullTask->RemoveAssignee(unit);
		}

		AddPower(unit);

		if (unit->GetCircuitDef()->IsAbleToFly()) {
			unit->GetUnit()->ExecuteCustomCommand(CMD_RETREAT, {2.0f});
			if (unit->GetCircuitDef()->GetMaxRange() > 600.0f) {  // armbrawl
				unit->GetUnit()->ExecuteCustomCommand(CMD_AIR_STRAFE, {0.0f});
			}
		}
	};
	auto attackerIdleHandler = [this](CCircuitUnit* unit) {
		// NOTE: Avoid instant task reassignment, though it may be not relevant for attackers
//		if (this->circuit->GetLastFrame() - unit->GetTaskFrame() > FRAMES_PER_SEC) {
			unit->GetTask()->OnUnitIdle(unit);
//		}
	};
	auto attackerDamagedHandler = [this](CCircuitUnit* unit, CEnemyUnit* attacker) {
		unit->GetTask()->OnUnitDamaged(unit, attacker);
	};
	auto attackerDestroyedHandler = [this](CCircuitUnit* unit, CEnemyUnit* attacker) {
		IUnitTask* task = unit->GetTask();
		task->OnUnitDestroyed(unit, attacker);  // can change task
		unit->GetTask()->RemoveAssignee(unit);  // Remove unit from IdleTask

		if (unit->GetUnit()->IsBeingBuilt()) {  // alternative: task == nullTask
			return;
		}

		DelPower(unit);
	};

	ReadConfig();
	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const Json::Value& retreats = root["retreat"];
	const float fighterRet = retreats["_fighter_"].asFloat();

	const CCircuitAI::CircuitDefs& allDefs = circuit->GetCircuitDefs();
	for (auto& kv : allDefs) {
		CCircuitDef* cdef = kv.second;
		if (!cdef->IsAttacker() || cdef->GetUnitDef()->IsBuilder()) {
			continue;
		}
		const std::map<std::string, std::string>& customParams = cdef->GetUnitDef()->GetCustomParams();
		auto it = customParams.find("is_drone");
		if ((it != customParams.end()) && (utils::string_to_int(it->second) == 1)) {
			continue;
		}
		if (cdef->IsMobile()) {
			unitDefId = kv.first;
			createdHandler[unitDefId] = attackerCreatedHandler;
			finishedHandler[unitDefId] = attackerFinishedHandler;
			idleHandler[unitDefId] = attackerIdleHandler;
			damagedHandler[unitDefId] = attackerDamagedHandler;
			destroyedHandler[unitDefId] = attackerDestroyedHandler;

			const char* name = cdef->GetUnitDef()->GetName();
			cdef->SetRetreat(retreats.get(name, fighterRet).asFloat());
		} else {
			WeaponDef* wd = cdef->GetUnitDef()->GetStockpileDef();
			if (wd == nullptr) {
				continue;
			}
			unitDefId = kv.first;
			finishedHandler[unitDefId] = defenceFinishedHandler;
			delete wd;
		}
	}

	/*
	 * raveparty handlers
	 */
	unitDefId = circuit->GetCircuitDef("raveparty")->GetId();
	finishedHandler[unitDefId] = [this](CCircuitUnit* unit) {
		unit->GetUnit()->SetTrajectory(1);
	};

//	/*
//	 * armrectr handlers
//	 */
//	unitDefId = attrib->GetUnitDefByName("armrectr")->GetUnitDefId();
//	idleHandler[unitDefId] = [this](CCircuitUnit* unit) {
//		fighterInfos.erase(unit);
//	};
//	damagedHandler[unitDefId] = [this](CCircuitUnit* unit, CEnemyUnit* attacker) {
//		if (attacker != nullptr) {
//			auto search = fighterInfos.find(unit);
//			if (search == fighterInfos.end()) {
//				Unit* u = unit->GetUnit();
//				std::vector<float> params;
//				params.push_back(2.0f);
//				u->ExecuteCustomCommand(CMD_PRIORITY, params);
//
//				const AIFloat3& pos = attacker->GetPos();
//				params.clear();
//				params.push_back(1.0f);  // 1: terraform_type, 1 == level
//				params.push_back(this->circuit->GetTeamId());  // 2: teamId
//				params.push_back(0.0f);  // 3: terraform type - 0 == Wall, else == Area
//				params.push_back(pos.y - 42.0f);  // 4: terraformHeight
//				params.push_back(1.0f);  // 5: number of control points
//				params.push_back(1.0f);  // 6: units count?
//				params.push_back(0.0f);  // 7: volumeSelection?
//				params.push_back(pos.x);  //  8: i + 0 control point x
//				params.push_back(pos.z);  //  9: i + 1 control point z
//				params.push_back(unit->GetId());  // 10: i + 2 unitId
//				u->ExecuteCustomCommand(CMD_TERRAFORM_INTERNAL, params);
//
//				fighterInfos[unit].isTerraforming = true;
//			}
//		}
//	};
//	destroyedHandler[unitDefId] = [this](CCircuitUnit* unit, CEnemyUnit* attacker) {
//		fighterInfos.erase(unit);
//	};

	defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();

	fightTasks.resize(static_cast<IFighterTask::FT>(IFighterTask::FightType::TASKS_COUNT));
}

CMilitaryManager::~CMilitaryManager()
{
	PRINT_DEBUG("Execute: %s\n", __PRETTY_FUNCTION__);
	for (std::set<IFighterTask*>& tasks : fightTasks) {
		utils::free_clear(tasks);
	}
	utils::free_clear(fightDeleteTasks);

	utils::free_clear(retreatTasks);
	utils::free_clear(retDeleteTasks);
}

int CMilitaryManager::UnitCreated(CCircuitUnit* unit, CCircuitUnit* builder)
{
	auto search = createdHandler.find(unit->GetCircuitDef()->GetId());
	if (search != createdHandler.end()) {
		search->second(unit, builder);
	}

	return 0; //signaling: OK
}

int CMilitaryManager::UnitFinished(CCircuitUnit* unit)
{
	auto search = finishedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != finishedHandler.end()) {
		search->second(unit);
	}

	return 0; //signaling: OK
}

int CMilitaryManager::UnitIdle(CCircuitUnit* unit)
{
	auto search = idleHandler.find(unit->GetCircuitDef()->GetId());
	if (search != idleHandler.end()) {
		search->second(unit);
	}

	return 0; //signaling: OK
}

int CMilitaryManager::UnitDamaged(CCircuitUnit* unit, CEnemyUnit* attacker)
{
	auto search = damagedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != damagedHandler.end()) {
		search->second(unit, attacker);
	}

	return 0; //signaling: OK
}

int CMilitaryManager::UnitDestroyed(CCircuitUnit* unit, CEnemyUnit* attacker)
{
	auto search = destroyedHandler.find(unit->GetCircuitDef()->GetId());
	if (search != destroyedHandler.end()) {
		search->second(unit, attacker);
	}

	return 0; //signaling: OK
}

IFighterTask* CMilitaryManager::EnqueueTask(IFighterTask::FightType type)
{
	IFighterTask* task;
	switch (type) {
		default:
		case IFighterTask::FightType::RALLY: {
			CEconomyManager* economyManager = circuit->GetEconomyManager();
			float power = economyManager->GetAvgMetalIncome() * economyManager->GetEcoFactor() * 32.0f;
			task = new CRallyTask(this, /*power*/1);  // TODO: pass enemy's threat
			break;
		}
		case IFighterTask::FightType::SCOUT: {
			task = new CScoutTask(this);
			break;
		}
		case IFighterTask::FightType::ATTACK: {
			task = new CAttackTask(this);
			break;
		}
		case IFighterTask::FightType::BOMB: {
			task = new CBombTask(this);
			break;
		}
		case IFighterTask::FightType::MELEE: {
			task = new CMeleeTask(this);
			break;
		}
		case IFighterTask::FightType::ARTY: {
			task = new CArtilleryTask(this);
			break;
		}
//		case IFighterTask::FightType::AA: {
//			task = new CAntiAirTask(this);
//			break;
//		}
	}

	fightTasks[static_cast<IFighterTask::FT>(type)].insert(task);
	return task;
}

IFighterTask* CMilitaryManager::EnqueueGuard(CCircuitUnit* vip)
{
	IFighterTask* task = new CGuardTask(this, vip, 1.0f);
	fightTasks[static_cast<IFighterTask::FT>(IFighterTask::FightType::GUARD)].insert(task);
	return task;
}

CRetreatTask* CMilitaryManager::EnqueueRetreat()
{
	CRetreatTask* task = new CRetreatTask(this);
	retreatTasks.insert(task);
	return task;
}

void CMilitaryManager::DequeueTask(IFighterTask* task, bool done)
{
	if (task->GetType() == IUnitTask::Type::FIGHTER) {
		std::set<IFighterTask*>& tasks = fightTasks[static_cast<IFighterTask::FT>(task->GetFightType())];
		auto it = tasks.find(task);
		if (it != tasks.end()) {
			tasks.erase(it);
			task->Close(done);
			fightDeleteTasks.insert(task);
		}
	} else {
		auto it = retreatTasks.find(task);
		if (it != retreatTasks.end()) {
			retreatTasks.erase(task);
			task->Close(done);
			retDeleteTasks.insert(task);
		}
	}
}

IUnitTask* CMilitaryManager::MakeTask(CCircuitUnit* unit)
{
	circuit->GetThreatMap()->SetThreatType(unit);
	const IFighterTask* task = nullptr;
	// FIXME: COMPATIBILITY
//	int frame = circuit->GetLastFrame();
//	AIFloat3 pos = unit->GetPos(frame);
//	STerrainMapArea* area = unit->GetArea();
//	CTerrainManager* terrainManager = circuit->GetTerrainManager();
//	CPathFinder* pathfinder = circuit->GetPathfinder();
//	terrainManager->CorrectPosition(pos);
//	pathfinder->SetMapData(unit, circuit->GetThreatMap(), frame);
//	const float maxSpeed = unit->GetUnit()->GetMaxSpeed() / pathfinder->GetSquareSize() * THREAT_BASE;
//	const int distance = std::max<int>(unit->GetCircuitDef()->GetMaxRange(), pathfinder->GetSquareSize());
//	float metric = std::numeric_limits<float>::max();
//
//	for (const std::set<IFighterTask*>& tasks : fightTasks) {
//		for (const IFighterTask* candidate : tasks) {
//			if (!candidate->CanAssignTo(unit)) {
//				continue;
//			}
//
//			// Check time-distance to target
//			float distCost;
//
//			const AIFloat3& tp = candidate->GetPosition();
//			AIFloat3 taskPos = (tp != -RgtVector) ? tp : pos;
//
//			if (!terrainManager->CanMoveToPos(area, taskPos)) {  // ensure that path always exists
//				continue;
//			}
//
//			distCost = std::max(pathfinder->PathCost(pos, taskPos, distance), THREAT_BASE);
//
//			if ((distCost < metric) && (distCost < MAX_TRAVEL_SEC * (maxSpeed * FRAMES_PER_SEC))) {
//				task = candidate;
//				metric = distCost;
//			}
//		}
//	}

	CCircuitDef::RoleM role =
		CCircuitDef::RoleMask::SCOUT |
		CCircuitDef::RoleMask::BOMBER |
		CCircuitDef::RoleMask::MELEE |
		CCircuitDef::RoleMask::ARTY |
		CCircuitDef::RoleMask::AA;
	if (!unit->GetCircuitDef()->IsRoleAny(role)) {
		for (const std::set<IFighterTask*>& tasks : fightTasks) {
			for (const IFighterTask* candidate : tasks) {
				if (!candidate->CanAssignTo(unit)) {
					continue;
				}
				task = candidate;
				break;
			}
			if (task != nullptr) {
				break;
			}
		}
	}
	// FIXME: COMPATIBILITY

	if (task == nullptr) {
		IFighterTask::FightType type;
		if (unit->GetCircuitDef()->IsRoleScout()) {
			type = IFighterTask::FightType::SCOUT;
		} else if (unit->GetCircuitDef()->IsRoleBomber()) {
			type = IFighterTask::FightType::BOMB;
		} else if (unit->GetCircuitDef()->IsRoleMelee()) {
			type = IFighterTask::FightType::MELEE;
		} else if (unit->GetCircuitDef()->IsRoleArty()) {
			type = IFighterTask::FightType::ARTY;
//		} else if (unit->GetCircuitDef()->IsRoleAA()) {
//			type = IFighterTask::FightType::AA;
		} else {
			type = IFighterTask::FightType::RALLY;
		}
		task = EnqueueTask(type);
	}

	return const_cast<IFighterTask*>(task);
}

void CMilitaryManager::AbortTask(IUnitTask* task)
{
	DequeueTask(static_cast<IFighterTask*>(task), false);
}

void CMilitaryManager::DoneTask(IUnitTask* task)
{
	DequeueTask(static_cast<IFighterTask*>(task), true);
}

void CMilitaryManager::FallbackTask(CCircuitUnit* unit)
{
}

void CMilitaryManager::MakeDefence(const AIFloat3& pos)
{
	CMetalManager* metalManager = circuit->GetMetalManager();
	int index = metalManager->FindNearestCluster(pos);
	if (index < 0) {
		return;
	}
	CCircuitDef* defDef;
	CEconomyManager* economyManager = circuit->GetEconomyManager();
	float maxCost = MIN_BUILD_SEC * std::min(economyManager->GetAvgMetalIncome(), economyManager->GetAvgEnergyIncome()) * economyManager->GetEcoFactor();
	CDefenceMatrix::SDefPoint* closestPoint = nullptr;
	float minDist = std::numeric_limits<float>::max();
	std::vector<CDefenceMatrix::SDefPoint>& points = defence->GetDefPoints(index);
	for (CDefenceMatrix::SDefPoint& defPoint : points) {
		if (defPoint.cost < maxCost) {
			float dist = defPoint.position.SqDistance2D(pos);
			if ((closestPoint == nullptr) || (dist < minDist)) {
				closestPoint = &defPoint;
				minDist = dist;
			}
		}
	}
	if (closestPoint == nullptr) {
		return;
	}
	CBuilderManager* builderManager = circuit->GetBuilderManager();
	float totalCost = .0f;
	IBuilderTask* parentTask = nullptr;
	bool isWater = circuit->GetTerrainManager()->IsWaterSector(pos);
//	std::array<const char*, 9> landDefenders = {"corllt", "corrl", "corrad", "corrl", "corhlt", "corrazor", "armnanotc", "cordoom", "corjamt"/*, "armanni", "corbhmth"*/};
//	std::array<const char*, 9> waterDefenders = {"turrettorp", "armsonar", "corllt", "corrad", "corrazor", "armnanotc", "turrettorp", "corhlt", "turrettorp"};
	std::array<const char*, 7> landDefenders = {"corllt", "corrl", "corrl", "corrl", "corhlt", "corrazor", "armnanotc"};
	std::array<const char*, 7> waterDefenders = {"turrettorp", "corrl", "turrettorp", "turrettorp", "corhlt", "corrazor", "armnanotc"};
	std::array<const char*, 7>& defenders = isWater ? waterDefenders : landDefenders;

	// Front-line porc
	bool isPorc = false;
	CThreatMap* threatMap = circuit->GetThreatMap();
	const CMetalData::Clusters& clusters = metalManager->GetClusters();
	const CMetalData::Metals& spots = metalManager->GetSpots();
	const CMetalData::Graph& clusterGraph = metalManager->GetGraph();
	CMetalData::Graph::out_edge_iterator outEdgeIt, outEdgeEnd;
	std::tie(outEdgeIt, outEdgeEnd) = boost::out_edges(index, clusterGraph);
	for (; (outEdgeIt != outEdgeEnd) && !isPorc; ++outEdgeIt) {
		const CMetalData::EdgeDesc& edgeId = *outEdgeIt;
		int idx0 = boost::target(edgeId, clusterGraph);
		if (metalManager->IsClusterFinished(idx0)) {
			continue;
		}
		// check if there is enemy neighbor
		for (int idx : clusters[idx0].idxSpots) {
			if (threatMap->GetAllThreatAt(spots[idx].position) > THREAT_MIN * 5) {
				isPorc = true;
				break;
			}
		}
	}
	unsigned num = isPorc ? 7 : 1;

	for (unsigned i = 0; i < num; ++i) {
		const char* name = defenders[i];
		defDef = circuit->GetCircuitDef(name);
		float defCost = defDef->GetCost();
		totalCost += defCost;
		if (totalCost <= closestPoint->cost) {
			continue;
		}
		if (totalCost < maxCost) {
			closestPoint->cost += defCost;
			bool isFirst = (parentTask == nullptr);
			IBuilderTask::Priority priority = isFirst ? IBuilderTask::Priority::HIGH : IBuilderTask::Priority::NORMAL;
			IBuilderTask* task = builderManager->EnqueueTask(priority, defDef, closestPoint->position,
															 IBuilderTask::BuildType::DEFENCE, defCost, SQUARE_SIZE * 32, isFirst);
			if (parentTask != nullptr) {
				parentTask->SetNextTask(task);
			}
			parentTask = task;
		} else {
			// TODO: Auto-sort defenders by cost OR remove break?
			break;
		}
	}

	// Build sensors
	auto checkSensor = [this, closestPoint, builderManager](IBuilderTask::BuildType type, CCircuitDef* cdef, float range) {
		bool isBuilt = false;
		auto friendlies = std::move(circuit->GetCallback()->GetFriendlyUnitsIn(closestPoint->position, range));
		for (Unit* au : friendlies) {
			if (au == nullptr) {
				continue;
			}
			UnitDef* udef = au->GetDef();
			CCircuitDef::Id defId = udef->GetUnitDefId();
			delete udef;
			if (defId == cdef->GetId()) {
				isBuilt = true;
				break;
			}
		}
		utils::free_clear(friendlies);
		if (!isBuilt) {
			IBuilderTask* task = nullptr;
			const float qdist = range * range;
			for (IBuilderTask* t : builderManager->GetTasks(type)) {
				if (closestPoint->position.SqDistance2D(t->GetTaskPos()) < qdist) {
					task = t;
					break;
				}
			}
			if (task == nullptr) {
				builderManager->EnqueueTask(IBuilderTask::Priority::NORMAL, cdef, closestPoint->position, type);
			}
		}
	};
	// radar
	defDef = circuit->GetCircuitDef("corrad");
	checkSensor(IBuilderTask::BuildType::RADAR, defDef, defDef->GetUnitDef()->GetRadarRadius() / SQRT_2);
	if (isWater) {  // sonar
		defDef = circuit->GetCircuitDef("armsonar");
		checkSensor(IBuilderTask::BuildType::SONAR, defDef, defDef->GetUnitDef()->GetSonarRadius());
	}
}

void CMilitaryManager::AbortDefence(CBDefenceTask* task)
{
	float defCost = task->GetBuildDef()->GetCost();
	CDefenceMatrix::SDefPoint* point = defence->GetDefPoint(task->GetPosition(), defCost);
	if (point != nullptr) {
		if ((task->GetTarget() == nullptr) && (point->cost >= defCost)) {
			point->cost -= defCost;
		}
		IBuilderTask* next = task->GetNextTask();
		while (next != nullptr) {
			if (next->GetBuildDef() != nullptr) {
				defCost = next->GetBuildDef()->GetCost();
			} else{
				defCost = next->GetCost();
			}
			if (point->cost >= defCost) {
				point->cost -= defCost;
			}
			next = next->GetNextTask();
		}
	}

}

bool CMilitaryManager::HasDefence(int cluster)
{
	// FIXME: COMPATIBILITY
	return true;
	// FIXME: COMPATIBILITY
	const std::vector<CDefenceMatrix::SDefPoint>& points = defence->GetDefPoints(cluster);
	for (const CDefenceMatrix::SDefPoint& defPoint : points) {
		if (defPoint.cost > .5f) {
			return true;
		}
	}
	return false;
}

AIFloat3 CMilitaryManager::GetScoutPosition(CCircuitUnit* unit)
{
	CTerrainManager* terrainManager = circuit->GetTerrainManager();
	STerrainMapArea* area = unit->GetArea();
	CMetalManager* metalManager = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalManager->GetSpots();
	decltype(scoutIdx) prevIdx = scoutIdx;
	while (scoutIdx < scoutPath.size()) {
		int index = scoutPath[scoutIdx++];
		if (!metalManager->IsMexInFinished(index) && terrainManager->CanMoveToPos(area, spots[index].position)) {
			return spots[index].position;
		}
	}
	scoutIdx = 0;
	while (scoutIdx < prevIdx) {
		int index = scoutPath[scoutIdx++];
		if (!metalManager->IsMexInFinished(index) && terrainManager->CanMoveToPos(area, spots[index].position)) {
			return spots[index].position;
		}
	}
//	++scoutIdx %= scoutPath.size();
	return -RgtVector;
}

IFighterTask* CMilitaryManager::AddDefendTask(int cluster)
{
	// FIXME: COMPATIBILITY
	return nullptr;
	// FIXME: COMPATIBILITY
	IFighterTask* task = clusterInfos[cluster].defence;
	if (task != nullptr) {
		return task;
	}

	const AIFloat3& pos = circuit->GetMetalManager()->GetClusters()[cluster].geoCentr;
//	task = EnqueueTask(IFighterTask::FightType::DEFEND, pos, 1.0f);
	task = new CDefendTask(this, pos, 1.0f);
	fightTasks[static_cast<IFighterTask::FT>(IFighterTask::FightType::DEFEND)].insert(task);
	clusterInfos[cluster].defence = task;
	return task;
}

IFighterTask* CMilitaryManager::DelDefendTask(const AIFloat3& pos)
{
	// FIXME: COMPATIBILITY
	return nullptr;
	// FIXME: COMPATIBILITY
	int index = circuit->GetMetalManager()->FindNearestCluster(pos);
	if (index < 0) {
		return nullptr;
	}

	return DelDefendTask(index);
}

IFighterTask* CMilitaryManager::DelDefendTask(int cluster)
{
	// FIXME: COMPATIBILITY
	return nullptr;
	// FIXME: COMPATIBILITY
	IFighterTask* task = clusterInfos[cluster].defence;
	if (task == nullptr) {
		return nullptr;
	}

	clusterInfos[cluster].defence = nullptr;
	return task;
}

//bool CMilitaryManager::IsNeedRole(CCircuitDef* cdef, CCircuitDef::RoleType type) const
//{
//	const SRoleInfo& info = roleInfos[type];
//	const float threat = circuit->GetThreatMap()->GetAirMetal(); ??
//	const float nextMetal = info.metal + cdef->GetCost();
//	return (threat * info.ratio > nextMetal * info.factor) && (nextMetal < info.maxPerc * metalArmy);
//}

bool CMilitaryManager::IsNeedAA(CCircuitDef* cdef) const
{
	const SRoleInfo& info = roleInfos[static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::AA)];
	const float airThreat = circuit->GetThreatMap()->GetAirMetal();
	const float nextMetalAA = info.metal + cdef->GetCost();
	return (airThreat * info.ratio > nextMetalAA * info.factor) && (nextMetalAA < info.maxPerc * metalArmy);
}

bool CMilitaryManager::IsNeedArty(CCircuitDef* cdef) const
{
	const SRoleInfo& info = roleInfos[static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::ARTY)];
	const float staticThreat = circuit->GetThreatMap()->GetStaticMetal();
	const float nextMetalArty = info.metal + cdef->GetCost();
	return (staticThreat * info.ratio > nextMetalArty * info.factor) && (nextMetalArty < info.maxPerc * metalArmy);
}

bool CMilitaryManager::IsNeedBigGun(CCircuitDef* cdef) const
{
	return metalArmy * circuit->GetEconomyManager()->GetEcoFactor() > cdef->GetCost();
}

void CMilitaryManager::ReadConfig()
{
	const Json::Value& root = circuit->GetSetupManager()->GetConfig();

	const Json::Value& ratio = root["response"];
	const float teamSize = circuit->GetAllyTeam()->GetSize();
	roleInfos.resize(static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::TOTAL_COUNT), {.0f});
	std::pair<const char*, CCircuitDef::RoleType> responseNames[] = {
		std::make_pair("raider",    CCircuitDef::RoleType::RAIDER),
		std::make_pair("riot",      CCircuitDef::RoleType::RIOT),
		std::make_pair("assault",   CCircuitDef::RoleType::ASSAULT),
		std::make_pair("skirmish",  CCircuitDef::RoleType::SKIRM),
		std::make_pair("anti_air",  CCircuitDef::RoleType::AA),
		std::make_pair("artillery", CCircuitDef::RoleType::ARTY)
	};
	for (const auto& pair : responseNames) {
		SRoleInfo& info = roleInfos[static_cast<CCircuitDef::RoleT>(pair.second)];
		const Json::Value& response = ratio[pair.first];
		if (response == Json::Value::null) {
			info.ratio   = 1.0f;
			info.maxPerc = 1.0f;
			info.factor  = teamSize;
		} else {
			info.ratio   = response.get("ratio", 1.0f).asFloat();
			info.maxPerc = response.get("max_percent", 1.0f).asFloat();
			const float step = response.get("eps_step", 1.0f).asFloat();
			info.factor  = (teamSize - 1.0f) * step + 1.0f;
		}
	}

	const Json::Value& retreats = root["retreat"];
	std::pair<const char*, void (CCircuitDef::*)(bool)> retreatNames[] = {
		std::make_pair("_siege_",     &CCircuitDef::SetSiege),
		std::make_pair("_hold_fire_", &CCircuitDef::SetHoldFire)
	};
	for (const auto& pair : retreatNames) {
		const Json::Value& siege = retreats[pair.first];
		for (unsigned i = 0; i < siege.size(); ++i) {
			CCircuitDef* udef = circuit->GetCircuitDef(siege[i].asCString());
			if (udef == nullptr) {
				continue;
			}
			(udef->*pair.second)(true);
		}
	}
}

void CMilitaryManager::Init()
{
	CMetalManager* metalManager = circuit->GetMetalManager();
	const CMetalData::Metals& spots = metalManager->GetSpots();
	const CMetalData::Clusters& clusters = metalManager->GetClusters();

	scoutPath.reserve(spots.size());
	for (unsigned i = 0; i < spots.size(); ++i) {
		scoutPath.push_back(i);
	}
	const AIFloat3& pos = circuit->GetSetupManager()->GetStartPos();
	auto compare = [&pos, &spots](int a, int b) {
		return pos.SqDistance2D(spots[a].position) > pos.SqDistance2D(spots[b].position);
	};
	std::sort(scoutPath.begin(), scoutPath.end(), compare);

	clusterInfos.resize(clusters.size(), {nullptr});

	CScheduler* scheduler = circuit->GetScheduler().get();
	const int interval = 4;
	const int offset = circuit->GetSkirmishAIId() % interval;
	scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateIdle, this), interval, offset + 0);
	scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateFight, this), interval / 2, offset + 1);
	scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateRetreat, this), interval, offset + 2);
}

void CMilitaryManager::Watchdog()
{
	PRINT_DEBUG("Execute: %s\n", __PRETTY_FUNCTION__);
	for (CCircuitUnit* unit : army) {
		auto commands = std::move(unit->GetUnit()->GetCurrentCommands());
		if (commands.empty()) {
			UnitIdle(unit);
		}
		utils::free_clear(commands);
	}
}

void CMilitaryManager::UpdateIdle()
{
	idleTask->Update();
}

void CMilitaryManager::UpdateRetreat()
{
	if (!retDeleteTasks.empty()) {
		for (auto task : retDeleteTasks) {
			retUpdateTasks.erase(task);
			delete task;
		}
		retDeleteTasks.clear();
	}

	auto it = retUpdateTasks.begin();
	unsigned int i = 0;
	while (it != retUpdateTasks.end()) {
		(*it)->Update();

		it = retUpdateTasks.erase(it);
		if (++i >= retUpdateSlice) {
			break;
		}
	}

	if (retUpdateTasks.empty()) {
		retUpdateTasks = retreatTasks;
		retUpdateSlice = retUpdateTasks.size() / TEAM_SLOWUPDATE_RATE;
	}
}

void CMilitaryManager::UpdateFight()
{
	if (!fightDeleteTasks.empty()) {
		for (auto task : fightDeleteTasks) {
			fightUpdateTasks.erase(task);
			delete task;
		}
		fightDeleteTasks.clear();
	}

	auto it = fightUpdateTasks.begin();
	unsigned int i = 0;
	while (it != fightUpdateTasks.end()) {
		(*it)->Update();

		it = fightUpdateTasks.erase(it);
		if (++i >= fightUpdateSlice) {
			break;
		}
	}

	if (fightUpdateTasks.empty()) {
		for (auto& tasks : fightTasks) {
			fightUpdateTasks.insert(tasks.begin(), tasks.end());
		}
		fightUpdateSlice = fightUpdateTasks.size() / TEAM_SLOWUPDATE_RATE;
	}
}

void CMilitaryManager::AddPower(CCircuitUnit* unit)
{
	army.insert(unit);

	CCircuitDef* cdef = unit->GetCircuitDef();
	const float cost = cdef->GetCost();
	assert(roleInfos.size() == static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::TOTAL_COUNT));
	for (CCircuitDef::RoleT i = 0; i < static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::TOTAL_COUNT); ++i) {
		if (cdef->IsRoleAny(CCircuitDef::GetMask(i))) {
			roleInfos[i].metal += cost;
		}
	}
	metalArmy += cost;
}

void CMilitaryManager::DelPower(CCircuitUnit* unit)
{
	army.erase(unit);

	CCircuitDef* cdef = unit->GetCircuitDef();
	const float cost = cdef->GetCost();
	assert(roleInfos.size() == static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::TOTAL_COUNT));
	for (CCircuitDef::RoleT i = 0; i < static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::TOTAL_COUNT); ++i) {
		if (cdef->IsRoleAny(CCircuitDef::GetMask(i))) {
			float& metal = roleInfos[i].metal;
			metal = std::max(metal - cost, .0f);
		}
	}
	metalArmy = std::max(metalArmy - cost, .0f);
}

} // namespace circuit
