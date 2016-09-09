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
#include "task/fighter/RaidTask.h"
#include "task/fighter/AttackTask.h"
#include "task/fighter/BombTask.h"
#include "task/fighter/ArtilleryTask.h"
#include "task/fighter/AntiAirTask.h"
#include "task/fighter/AntiHeavyTask.h"
#include "task/fighter/SupportTask.h"
#include "task/static/SuperTask.h"
#include "terrain/TerrainManager.h"
#include "terrain/ThreatMap.h"
#include "terrain/PathFinder.h"
#include "unit/EnemyUnit.h"
#include "CircuitAI.h"
#include "util/Scheduler.h"
#include "util/utils.h"
#include "json/json.h"

#include "OOAICallback.h"
#include "AISCommands.h"
#include "Command.h"
#include "Log.h"
#include "Map.h"

namespace circuit {

using namespace springai;

CMilitaryManager::CMilitaryManager(CCircuitAI* circuit)
		: IUnitModule(circuit)
		, fightIterator(0)
		, scoutIdx(0)
		, armyCost(.0f)
		, radarDef(nullptr)
		, sonarDef(nullptr)
{
	CScheduler* scheduler = circuit->GetScheduler().get();
	scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::Watchdog, this),
							FRAMES_PER_SEC * 60,
							circuit->GetSkirmishAIId() * WATCHDOG_COUNT + 12);
	scheduler->RunTaskAt(std::make_shared<CGameTask>(&CMilitaryManager::Init, this));

	/*
	 * Defence handlers
	 */
	auto defenceFinishedHandler = [this](CCircuitUnit* unit) {
		TRY_UNIT(this->circuit, unit,
			unit->GetUnit()->Stockpile(UNIT_COMMAND_OPTION_SHIFT_KEY | UNIT_COMMAND_OPTION_CONTROL_KEY);
		)
	};
	auto defenceDestroyedHandler = [this](CCircuitUnit* unit, CEnemyUnit* attacker) {
		int frame = this->circuit->GetLastFrame();
		float defCost = unit->GetCircuitDef()->GetCost();
		CDefenceMatrix::SDefPoint* point = defence->GetDefPoint(unit->GetPos(frame), defCost);
		if (point != nullptr) {
			point->cost -= defCost;
		}
	};
	for (const CCircuitDef* cdef : defenderDefs) {
		destroyedHandler[cdef->GetId()] = defenceDestroyedHandler;
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
		this->circuit->AddActionUnit(unit);

		AddPower(unit);

		TRY_UNIT(this->circuit, unit,
			if (unit->GetCircuitDef()->IsAbleToFly()) {
				if (unit->GetCircuitDef()->IsAttrNoStrafe()) {
					unit->GetUnit()->ExecuteCustomCommand(CMD_AIR_STRAFE, {0.0f});
				}
				if (unit->GetCircuitDef()->IsRoleMine()) {
					unit->GetUnit()->SetIdleMode(1);
				}
			}
			if (unit->GetCircuitDef()->IsRoleArty()) {
				unit->GetUnit()->ExecuteCustomCommand(CMD_DONT_FIRE_AT_RADAR, {0.0f});
			}
		)
	};
	auto attackerIdleHandler = [this](CCircuitUnit* unit) {
		// NOTE: Avoid instant task reassignment, though it may be not relevant for attackers
		if (this->circuit->GetLastFrame() > unit->GetTaskFrame()/* + FRAMES_PER_SEC*/) {
			unit->GetTask()->OnUnitIdle(unit);
		}
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

	/*
	 * Superweapon handlers
	 */
	auto superCreatedHandler = [this](CCircuitUnit* unit, CCircuitUnit* builder) {
		if (unit->GetTask() == nullptr) {
			unit->SetManager(this);
			nullTask->AssignTo(unit);
		}
	};
	auto superFinishedHandler = [this](CCircuitUnit* unit) {
		IFighterTask* task = EnqueueTask(IFighterTask::FightType::SUPER);
		if (unit->GetTask() == nullptr) {
			unit->SetManager(this);
			task->AssignTo(unit);
			task->Execute(unit);
		} else {
			nullTask->RemoveAssignee(unit);
			AssignTask(unit, task);
		}
		this->circuit->AddActionUnit(unit);

		TRY_UNIT(this->circuit, unit,
			unit->GetUnit()->SetFireState(0);
			unit->GetUnit()->SetTrajectory(1);
			if (unit->GetCircuitDef()->IsAttrStock()) {
				unit->GetUnit()->Stockpile(UNIT_COMMAND_OPTION_SHIFT_KEY | UNIT_COMMAND_OPTION_CONTROL_KEY);
				unit->GetUnit()->ExecuteCustomCommand(CMD_MISC_PRIORITY, {2.0f});
			}
		)
	};
	auto superDestroyedHandler = [this](CCircuitUnit* unit, CEnemyUnit* attacker) {
		IUnitTask* task = unit->GetTask();
		task->OnUnitDestroyed(unit, attacker);  // can change task
		unit->GetTask()->RemoveAssignee(unit);  // Remove unit from IdleTask
	};

	/*
	 * Defend buildings handler
	 */
	auto structDamagedHandler = [this](CCircuitUnit* unit, CEnemyUnit* attacker) {
		const AIFloat3& pos = unit->GetPos(this->circuit->GetLastFrame());
		CTerrainManager* terrainManager = this->circuit->GetTerrainManager();
		CDefendTask* defendTask = nullptr;
		float minSqDist = std::numeric_limits<float>::max();
		const std::set<IFighterTask*>& tasks = GetTasks(IFighterTask::FightType::DEFEND);
		for (IFighterTask* task : tasks) {
			CDefendTask* dt = static_cast<CDefendTask*>(task);
			if (dt->GetTarget() != nullptr) {
				continue;
			}
			const float sqDist = pos.SqDistance2D(dt->GetPosition());
			if ((minSqDist < sqDist) && (terrainManager->CanMoveToPos(dt->GetLeader()->GetArea(), pos))) {
				minSqDist = sqDist;
				defendTask = dt;
			}
		}
		if (defendTask != nullptr) {
			defendTask->SetPosition(pos);
			defendTask->SetWantedTarget(attacker);
		}
	};

	ReadConfig();

	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const float fighterRet = root["retreat"].get("fighter", 0.5f).asFloat();
	float maxRadarDivCost = 0.f;
	float maxSonarDivCost = 0.f;
	CCircuitDef* commDef = circuit->GetSetupManager()->GetCommChoice();

	const CCircuitAI::CircuitDefs& allDefs = circuit->GetCircuitDefs();
	for (auto& kv : allDefs) {
		CCircuitDef::Id unitDefId = kv.first;
		CCircuitDef* cdef = kv.second;
		if (cdef->GetUnitDef()->IsBuilder()) {
			damagedHandler[unitDefId] = structDamagedHandler;
			continue;
		}
		const std::map<std::string, std::string>& customParams = cdef->GetUnitDef()->GetCustomParams();
		auto it = customParams.find("is_drone");
		if ((it != customParams.end()) && (utils::string_to_int(it->second) == 1)) {
			continue;
		}
		if (cdef->IsMobile()) {
			createdHandler[unitDefId] = attackerCreatedHandler;
			finishedHandler[unitDefId] = attackerFinishedHandler;
			idleHandler[unitDefId] = attackerIdleHandler;
			damagedHandler[unitDefId] = attackerDamagedHandler;
			destroyedHandler[unitDefId] = attackerDestroyedHandler;

			if (cdef->GetRetreat() < 0.f) {
				cdef->SetRetreat(fighterRet);
			}
		} else {
			damagedHandler[unitDefId] = structDamagedHandler;
			if (cdef->IsAttacker()) {
				if (cdef->IsAttrSuper()) {
					createdHandler[unitDefId] = superCreatedHandler;
					finishedHandler[unitDefId] = superFinishedHandler;
					destroyedHandler[unitDefId] = superDestroyedHandler;
				} else if (cdef->IsAttrStock()) {
					finishedHandler[unitDefId] = defenceFinishedHandler;
				}
			}
			if (commDef->CanBuild(cdef)) {
				float range = cdef->GetUnitDef()->GetRadarRadius();
				float areaDivCost = M_PI * SQUARE(range) / cdef->GetCost();
				if (maxRadarDivCost < areaDivCost) {
					maxRadarDivCost = areaDivCost;
					radarDef = cdef;
				}
				range = cdef->GetUnitDef()->GetSonarRadius();
				areaDivCost = M_PI * SQUARE(range) / cdef->GetCost();
				if (maxSonarDivCost < areaDivCost) {
					maxSonarDivCost = areaDivCost;
					sonarDef = cdef;
				}
			}
		}
	}

	defence = circuit->GetAllyTeam()->GetDefenceMatrix().get();

	fightTasks.resize(static_cast<IFighterTask::FT>(IFighterTask::FightType::_SIZE_));

	enemyPos = AIFloat3(circuit->GetTerrainManager()->GetTerrainWidth() / 2, 0, circuit->GetTerrainManager()->GetTerrainHeight() / 2);
	enemyGroups.push_back(SEnemyGroup(enemyPos));
}

CMilitaryManager::~CMilitaryManager()
{
	PRINT_DEBUG("Execute: %s\n", __PRETTY_FUNCTION__);
	utils::free_clear(fightUpdates);
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
//			CEconomyManager* economyManager = circuit->GetEconomyManager();
//			float power = economyManager->GetAvgMetalIncome() * economyManager->GetEcoFactor() * 32.0f;
			task = new CRallyTask(this, /*power*/1);  // TODO: pass enemy's threat
			break;
		}
		case IFighterTask::FightType::DEFEND: {
			task = new CDefendTask(this, circuit->GetSetupManager()->GetBasePos(), maxGuards);
			break;
		}
		case IFighterTask::FightType::SCOUT: {
			task = new CScoutTask(this);
			break;
		}
		case IFighterTask::FightType::RAID: {
			task = new CRaidTask(this);
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
		case IFighterTask::FightType::ARTY: {
			task = new CArtilleryTask(this);
			break;
		}
		case IFighterTask::FightType::AA: {
			task = new CAntiAirTask(this);
			break;
		}
		case IFighterTask::FightType::AH: {
			task = new CAntiHeavyTask(this);
			break;
		}
		case IFighterTask::FightType::SUPPORT: {
			task = new CSupportTask(this);
			break;
		}
		case IFighterTask::FightType::SUPER: {
			task = new CSuperTask(this);
			break;
		}
	}

	fightTasks[static_cast<IFighterTask::FT>(type)].insert(task);
	fightUpdates.push_back(task);
	return task;
}

IFighterTask* CMilitaryManager::EnqueueGuard(CCircuitUnit* vip)
{
	IFighterTask* task = new CFGuardTask(this, vip, 1.0f);
	fightTasks[static_cast<IFighterTask::FT>(IFighterTask::FightType::GUARD)].insert(task);
	fightUpdates.push_back(task);
	return task;
}

CRetreatTask* CMilitaryManager::EnqueueRetreat()
{
	CRetreatTask* task = new CRetreatTask(this);
	fightUpdates.push_back(task);
	return task;
}

void CMilitaryManager::DequeueTask(IFighterTask* task, bool done)
{
	if (task->GetType() == IUnitTask::Type::FIGHTER) {
		fightTasks[static_cast<IFighterTask::FT>(task->GetFightType())].erase(task);
	}
	task->Dead();
	task->Close(done);
}

IUnitTask* CMilitaryManager::MakeTask(CCircuitUnit* unit)
{
	// FIXME: Make central task assignment system.
	//        MilitaryManager should decide what tasks to merge.
	static const std::map<CCircuitDef::RoleT, IFighterTask::FightType> types = {
		{static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::SCOUT),   IFighterTask::FightType::SCOUT},
		{static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::RAIDER),  IFighterTask::FightType::RAID},
		{static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::ARTY),    IFighterTask::FightType::ARTY},
		{static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::AA),      IFighterTask::FightType::AA},
		{static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::AH),      IFighterTask::FightType::AH},
		{static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::BOMBER),  IFighterTask::FightType::BOMB},
		{static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::SUPPORT), IFighterTask::FightType::SUPPORT},
		{static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::MINE),    IFighterTask::FightType::SCOUT},  // FIXME
	};
	IFighterTask::FightType type;
	CCircuitDef* cdef = unit->GetCircuitDef();
	if (cdef->IsRoleSupport()) {
		type = IFighterTask::FightType::SUPPORT;
		if (cdef->IsAttacker() && GetTasks(IFighterTask::FightType::ATTACK).empty()) {
			type = IFighterTask::FightType::DEFEND;
		}
	} else {
		auto it = types.find(cdef->GetMainRole());
		if (it != types.end()) {
			type = it->second;
			switch (type) {
				case IFighterTask::FightType::RAID: {
					if (cdef->IsRoleScout() && (GetTasks(IFighterTask::FightType::SCOUT).size() < maxScouts)) {
						type = IFighterTask::FightType::SCOUT;
					}
				} break;
				case IFighterTask::FightType::AH: {
					if (!cdef->IsRoleMine() && (GetEnemyCost(CCircuitDef::RoleType::HEAVY) < 1.f)) {
						type = IFighterTask::FightType::ATTACK;
					}
				} break;
				default: break;
			}
		} else {
			const bool isDefend = GetTasks(IFighterTask::FightType::ATTACK).empty() && !cdef->IsRoleHeavy();
			type = isDefend ? IFighterTask::FightType::DEFEND : IFighterTask::FightType::ATTACK;
		}
	}
	IFighterTask* task = EnqueueTask(type);

	return task;
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
	CEconomyManager* em = circuit->GetEconomyManager();
	const float metalIncome = std::min(em->GetAvgMetalIncome(), em->GetAvgEnergyIncome()) * em->GetEcoFactor();
	float maxCost = MIN_BUILD_SEC * amountFactor * metalIncome;
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
	// NOTE: circuit->GetTerrainManager()->IsWaterSector(pos) checks whole sector
	//       but water recognized as height < 0
	bool isWater = circuit->GetMap()->GetElevationAt(pos.x, pos.z) < -SQUARE_SIZE * 5;
	std::vector<CCircuitDef*>& defenders = isWater ? waterDefenders : landDefenders;

	// Front-line porc
	bool isPorc = metalManager->GetMaxIncome() > metalManager->GetAvgIncome() + 1.f;
	if (isPorc) {
		const float income = (metalManager->GetAvgIncome() + metalManager->GetMaxIncome()) * 0.5f;
		int spotId = metalManager->FindNearestSpot(pos);
		isPorc = metalManager->GetSpots()[spotId].income > income;
	}
	if (!isPorc) {
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
				if (threatMap->GetAllThreatAt(spots[idx].position) > THREAT_MIN * 2) {
					isPorc = true;
					break;
				}
			}
		}
	}
	unsigned num = std::min<unsigned>(isPorc ? defenders.size() : preventCount, defenders.size());

	AIFloat3 backDir = circuit->GetSetupManager()->GetBasePos() - closestPoint->position;
	AIFloat3 backPos = closestPoint->position + backDir.Normalize2D() * SQUARE_SIZE * 8;

	for (unsigned i = 0; i < num; ++i) {
		CCircuitDef* defDef = defenders[i];
		if (!defDef->IsAvailable()) {
			continue;
		}
		float defCost = defDef->GetCost();
		totalCost += defCost;
		if (totalCost <= closestPoint->cost) {
			continue;
		}
		if (totalCost < maxCost) {
			closestPoint->cost += defCost;
			bool isFirst = (parentTask == nullptr);
			IBuilderTask::Priority priority = isFirst ? IBuilderTask::Priority::HIGH : IBuilderTask::Priority::NORMAL;
			const AIFloat3& buildPos = defDef->IsAttacker() ? closestPoint->position : backPos;
			IBuilderTask* task = builderManager->EnqueueTask(priority, defDef, buildPos,
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
	auto checkSensor = [this, &backPos, builderManager](IBuilderTask::BuildType type, CCircuitDef* cdef, float range) {
		bool isBuilt = false;
		auto friendlies = std::move(circuit->GetCallback()->GetFriendlyUnitsIn(backPos, range));
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
				if (backPos.SqDistance2D(t->GetTaskPos()) < qdist) {
					task = t;
					break;
				}
			}
			if (task == nullptr) {
				builderManager->EnqueueTask(IBuilderTask::Priority::NORMAL, cdef, backPos, type);
			}
		}
	};
	// radar
	if ((radarDef != nullptr) && radarDef->IsAvailable() && (radarDef->GetCost() < maxCost)) {
		checkSensor(IBuilderTask::BuildType::RADAR, radarDef, radarDef->GetUnitDef()->GetRadarRadius() / SQRT_2);
	}
	if (isWater && (sonarDef != nullptr) && sonarDef->IsAvailable() && (sonarDef->GetCost() < maxCost)) {  // sonar
		checkSensor(IBuilderTask::BuildType::SONAR, sonarDef, sonarDef->GetUnitDef()->GetSonarRadius());
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
	// FIXME: Resume fighter/DefendTask experiment
	return true;
	// FIXME: Resume fighter/DefendTask experiment

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
	// FIXME: Resume fighter/DefendTask experiment
	return nullptr;
	// FIXME: Resume fighter/DefendTask experiment

	IFighterTask* task = clusterInfos[cluster].defence;
	if (task != nullptr) {
		return task;
	}

	const AIFloat3& pos = circuit->GetMetalManager()->GetClusters()[cluster].geoCentr;
//	task = EnqueueTask(IFighterTask::FightType::DEFEND, pos, 1);
	task = new CDefendTask(this, pos, 1);
	fightTasks[static_cast<IFighterTask::FT>(IFighterTask::FightType::DEFEND)].insert(task);
	clusterInfos[cluster].defence = task;
	return task;
}

IFighterTask* CMilitaryManager::DelDefendTask(const AIFloat3& pos)
{
	// FIXME: Resume fighter/DefendTask experiment
	return nullptr;
	// FIXME: Resume fighter/DefendTask experiment

	int index = circuit->GetMetalManager()->FindNearestCluster(pos);
	if (index < 0) {
		return nullptr;
	}

	return DelDefendTask(index);
}

IFighterTask* CMilitaryManager::DelDefendTask(int cluster)
{
	// FIXME: Resume fighter/DefendTask experiment
	return nullptr;
	// FIXME: Resume fighter/DefendTask experiment

	IFighterTask* task = clusterInfos[cluster].defence;
	if (task == nullptr) {
		return nullptr;
	}

	clusterInfos[cluster].defence = nullptr;
	return task;
}

void CMilitaryManager::AddEnemyCost(const CEnemyUnit* e)
{
	CCircuitDef* cdef = e->GetCircuitDef();
	assert(cdef != nullptr);

	enemyCosts[cdef->GetEnemyRole()] += e->GetCost();
}

void CMilitaryManager::DelEnemyCost(const CEnemyUnit* e)
{
	CCircuitDef* cdef = e->GetCircuitDef();
	assert(cdef != nullptr);

	float& metal = enemyCosts[cdef->GetEnemyRole()];
	metal = std::max(metal - e->GetCost(), 0.f);
}

float CMilitaryManager::RoleProbability(const CCircuitDef* cdef) const
{
	const SRoleInfo& info = roleInfos[cdef->GetMainRole()];
	float maxProb = 0.f;
	for (const SRoleInfo::SVsInfo& vs : info.vs) {
		const float enemyMetal = GetEnemyCost(vs.role);
		const float nextMetal = info.cost + cdef->GetCost();
		const float prob = enemyMetal / (info.cost + 1.f) * vs.importance;
		if ((prob > maxProb) &&
			(enemyMetal * vs.ratio >= nextMetal * info.factor) &&
			(nextMetal <= (armyCost + cdef->GetCost()) * info.maxPerc))
		{
			maxProb = prob;
		}
	}
	return maxProb;
}

bool CMilitaryManager::IsNeedBigGun(const CCircuitDef* cdef) const
{
	return armyCost * circuit->GetEconomyManager()->GetEcoFactor() > cdef->GetCost();
}

AIFloat3 CMilitaryManager::GetBigGunPos(CCircuitDef* bigDef) const
{
	CTerrainManager* terrainManager = circuit->GetTerrainManager();
	AIFloat3 pos = circuit->GetSetupManager()->GetBasePos();
	if (bigDef->GetMaxRange() < std::max(terrainManager->GetTerrainWidth(), terrainManager->GetTerrainHeight())) {
		CMetalManager* metalManager = circuit->GetMetalManager();
		const CMetalData::Clusters& clusters = metalManager->GetClusters();
		unsigned size = 1;
		for (unsigned i = 0; i < clusters.size(); ++i) {
			if (metalManager->IsClusterFinished(i)) {
				pos += clusters[i].geoCentr;
				++size;
			}
		}
		pos /= size;
	}
	return pos;
}

void CMilitaryManager::UpdateDefenceTasks()
{
	/*
	 * Defend expansion
	 */
	const std::set<IFighterTask*>& tasks = GetTasks(IFighterTask::FightType::DEFEND);
	if (tasks.empty()) {
		return;
	}
	CMetalManager* mm = circuit->GetMetalManager();
	CEconomyManager* em = circuit->GetEconomyManager();
	CTerrainManager* tm = circuit->GetTerrainManager();
	const CMetalData::Metals& spots = mm->GetSpots();
	const CMetalData::Clusters& clusters = mm->GetClusters();
	for (IFighterTask* task : tasks) {
		CDefendTask* dt = static_cast<CDefendTask*>(task);
		if (dt->GetTarget() != nullptr) {
			continue;
		}
		STerrainMapArea* area = dt->GetLeader()->GetArea();
		CMetalData::MetalPredicate predicate = [em, tm, area, &spots, &clusters](const CMetalData::MetalNode& v) {
			const CMetalData::MetalIndices& idcs = clusters[v.second].idxSpots;
			for (int idx : idcs) {
				if (!em->IsOpenSpot(idx) && tm->CanMoveToPos(area, spots[idx].position)) {
					return true;
				}
			}
			return false;
		};
		AIFloat3 center(tm->GetTerrainWidth() / 2, 0, tm->GetTerrainHeight() / 2);
		int index = mm->FindNearestCluster(center, predicate);
		if (index >= 0) {
			dt->SetPosition(clusters[index].geoCentr);
		}
	}

	// TODO: Porc push
}

void CMilitaryManager::ReadConfig()
{
	const Json::Value& root = circuit->GetSetupManager()->GetConfig();
	const std::string& cfgName = circuit->GetSetupManager()->GetConfigName();
	CCircuitDef::RoleName& roleNames = CCircuitDef::GetRoleNames();

	const Json::Value& responses = root["response"];
	const float teamSize = circuit->GetAllyTeam()->GetSize();
	roleInfos.resize(static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::_SIZE_), {.0f});
	for (const auto& pair : roleNames) {
		SRoleInfo& info = roleInfos[static_cast<CCircuitDef::RoleT>(pair.second)];
		const Json::Value& response = responses[pair.first];

		if (response.isNull()) {
			info.maxPerc = 1.0f;
			info.factor  = teamSize;
			continue;
		}

		info.maxPerc = response.get("max_percent", 1.0f).asFloat();
		const float step = response.get("eps_step", 1.0f).asFloat();
		info.factor  = (teamSize - 1.0f) * step + 1.0f;

		const Json::Value& vs = response["vs"];
		const Json::Value& ratio = response["ratio"];
		const Json::Value& importance = response["importance"];
		for (unsigned i = 0; i < vs.size(); ++i) {
			const std::string& roleName = vs[i].asString();
			auto it = roleNames.find(roleName);
			if (it == roleNames.end()) {
				circuit->LOG("CONFIG %s: response %s vs unknown role '%s'", cfgName.c_str(), pair.first.c_str(), roleName.c_str());
				continue;
			}
			float rat = ratio.get(i, 1.0f).asFloat();
			float imp = importance.get(i, 1.0f).asFloat();
			info.vs.push_back(SRoleInfo::SVsInfo(roleNames[roleName], rat, imp));
		}
	}

	const Json::Value& quotas = root["quota"];
	maxScouts = quotas.get("scout", 3).asUInt();
	maxRaiders = quotas.get("raider", 5).asUInt();
	maxGuards = quotas.get("guard", 2).asUInt();

	const Json::Value& porc = root["porcupine"];
	const Json::Value& defs = porc["unit"];
	defenderDefs.reserve(defs.size());
	for (const Json::Value& def : defs) {
		CCircuitDef* cdef = circuit->GetCircuitDef(def.asCString());
		if (cdef == nullptr) {
			circuit->LOG("CONFIG %s: has unknown UnitDef '%s'", cfgName.c_str(), def.asCString());
		} else {
			defenderDefs.push_back(cdef);
		}
	}
	const Json::Value& land = porc["land"];
	landDefenders.reserve(land.size());
	for (const Json::Value& idx : land) {
		unsigned index = idx.asUInt();
		if (index < defenderDefs.size()) {
			landDefenders.push_back(defenderDefs[index]);
		}
	}
	const Json::Value& watr = porc["water"];
	waterDefenders.reserve(watr.size());
	for (const Json::Value& idx : watr) {
		unsigned index = idx.asUInt();
		if (index < defenderDefs.size()) {
			waterDefenders.push_back(defenderDefs[index]);
		}
	}

	preventCount = porc.get("prevent", 1).asUInt();
	const Json::Value& amount = porc["amount"];
	const Json::Value& amOff = amount["offset"];
	const Json::Value& amFac = amount["factor"];
	const Json::Value& amMap = amount["map"];
	const float minOffset = amOff.get((unsigned)0, -0.2f).asFloat();
	const float maxOffset = amOff.get((unsigned)1, 0.2f).asFloat();
	const float offset = (float)rand() / RAND_MAX * (maxOffset - minOffset) + minOffset;
	const float minFactor = amFac.get((unsigned)0, 2.0f).asFloat();
	const float maxFactor = amFac.get((unsigned)1, 1.0f).asFloat();
	const float minMap = amMap.get((unsigned)0, 8.0f).asFloat();
	const float maxMap = amMap.get((unsigned)1, 24.0f).asFloat();
	const float mapSize = (circuit->GetMap()->GetWidth() / 64) * (circuit->GetMap()->GetHeight() / 64);
	amountFactor = (maxFactor - minFactor) / (SQUARE(maxMap) - SQUARE(minMap)) * (mapSize - SQUARE(minMap)) + minFactor + offset;
//	amountFactor = std::max(amountFactor, 0.f);

	const Json::Value& base = porc["base"];
	baseDefence.reserve(base.size());
	for (const Json::Value& pair : base) {
		unsigned index = pair.get((unsigned)0, -1).asUInt();
		if (index >= defenderDefs.size()) {
			continue;
		}
		int frame = pair.get((unsigned)1, 0).asInt();
		baseDefence.push_back(std::make_pair(defenderDefs[index], frame));
	}

	const Json::Value& super = porc["superweapon"];
	const Json::Value& items = super["unit"];
	const Json::Value& probs = super["weight"];
	struct SSuperInfo {
		CCircuitDef* cdef;
		float prob;
	};
	std::vector<SSuperInfo> supers;
	supers.reserve(items.size());
	float magnitude = 0.f;
	for (unsigned i = 0; i < items.size(); ++i) {
		SSuperInfo si;
		si.cdef = circuit->GetCircuitDef(items[i].asCString());
		if (si.cdef == nullptr) {
			circuit->LOG("CONFIG %s: has unknown UnitDef '%s'", cfgName.c_str(), items[i].asCString());
			continue;
		}
		si.cdef->AddAttribute(CCircuitDef::AttrType::SUPER);
		si.prob = probs.get(i, 1.f).asFloat();
		magnitude += si.prob;
		supers.push_back(si);
	}
	if (!supers.empty()) {
		unsigned choice = 0;
		float dice = (float)rand() / RAND_MAX * magnitude;
		float total = .0f;
		for (unsigned i = 0; i < supers.size(); ++i) {
			total += supers[i].prob;
			if (dice < total) {
				choice = i;
				break;
			}
		}
		bigGunDef = supers[choice].cdef;
	}

	defaultPorc = circuit->GetCircuitDef(porc.get("default", "").asCString());
	if (defaultPorc == nullptr) {
		defaultPorc = circuit->GetEconomyManager()->GetDefaultDef();
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
	scheduler->RunTaskEvery(std::make_shared<CGameTask>(&CMilitaryManager::UpdateDefenceTasks, this), FRAMES_PER_SEC * 10, offset + 2);
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

void CMilitaryManager::UpdateFight()
{
	if (fightIterator >= fightUpdates.size()) {
		fightIterator = 0;
	}

	// stagger the Update's
	unsigned int n = (fightUpdates.size() / TEAM_SLOWUPDATE_RATE) + 1;

	while ((fightIterator < fightUpdates.size()) && (n != 0)) {
		IUnitTask* task = fightUpdates[fightIterator];
		if (task->IsDead()) {
			fightUpdates[fightIterator] = fightUpdates.back();
			fightUpdates.pop_back();
			delete task;
		} else {
			task->Update();
			++fightIterator;
			n--;
		}
	}
}

void CMilitaryManager::AddPower(CCircuitUnit* unit)
{
	army.insert(unit);

	CCircuitDef* cdef = unit->GetCircuitDef();
	const float cost = cdef->GetCost();
	assert(roleInfos.size() == static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::_SIZE_));
	for (CCircuitDef::RoleT i = 0; i < static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::_SIZE_); ++i) {
		if (cdef->IsRoleAny(CCircuitDef::GetMask(i))) {
			roleInfos[i].cost += cost;
			roleInfos[i].units.insert(unit);
		}
	}
	armyCost += cost;
}

void CMilitaryManager::DelPower(CCircuitUnit* unit)
{
	army.erase(unit);

	CCircuitDef* cdef = unit->GetCircuitDef();
	const float cost = cdef->GetCost();
	assert(roleInfos.size() == static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::_SIZE_));
	for (CCircuitDef::RoleT i = 0; i < static_cast<CCircuitDef::RoleT>(CCircuitDef::RoleType::_SIZE_); ++i) {
		if (cdef->IsRoleAny(CCircuitDef::GetMask(i))) {
			float& metal = roleInfos[i].cost;
			metal = std::max(metal - cost, .0f);
			roleInfos[i].units.erase(unit);
		}
	}
	armyCost = std::max(armyCost - cost, .0f);
}

/*
 * 2d only, ignores y component.
 * @see KAIK/AttackHandler::KMeansIteration for general reference
 */
void CMilitaryManager::KMeansIteration()
{
	const CCircuitAI::EnemyUnits& units = circuit->GetEnemyUnits();
	// calculate a new K. change the formula to adjust max K, needs to be 1 minimum.
	constexpr int KMEANS_BASE_MAX_K = 32;
	int newK = std::min(KMEANS_BASE_MAX_K, 1 + (int)sqrtf(units.size()));

	// change the number of means according to newK
	assert(newK > 0/* && enemyGoups.size() > 0*/);
	// add a new means, just use one of the positions
	AIFloat3 newMeansPosition = units.begin()->second->GetPos();
//	newMeansPosition.y = circuit->GetMap()->GetElevationAt(newMeansPosition.x, newMeansPosition.z) + K_MEANS_ELEVATION;
	enemyGroups.resize(newK, SEnemyGroup(newMeansPosition));

	// check all positions and assign them to means, complexity n*k for one iteration
	std::vector<int> unitsClosestMeanID(units.size(), -1);
	std::vector<int> numUnitsAssignedToMean(newK, 0);

	{
		int i = 0;
		for (const auto& kv : units) {
			AIFloat3 unitPos = kv.second->GetPos();
			float closestDistance = std::numeric_limits<float>::max();
			int closestIndex = -1;

			for (int m = 0; m < newK; m++) {
				const AIFloat3& mean = enemyGroups[m].pos;
				float distance = unitPos.SqDistance2D(mean);

				if (distance < closestDistance) {
					closestDistance = distance;
					closestIndex = m;
				}
			}

			// position i is closest to the mean at closestIndex
			unitsClosestMeanID[i++] = closestIndex;
			numUnitsAssignedToMean[closestIndex]++;
		}
	}

	// change the means according to which positions are assigned to them
	// use meanAverage for indexes with 0 pos'es assigned
	// make a new means list
//	std::vector<AIFloat3> newMeans(newK, ZeroVector);
	std::vector<SEnemyGroup>& newMeans = enemyGroups;
	for (unsigned i = 0; i < newMeans.size(); i++) {
		SEnemyGroup& eg = newMeans[i];
		eg.units.clear();
		eg.units.reserve(numUnitsAssignedToMean[i]);
		eg.pos = ZeroVector;
		std::fill(eg.roleCosts.begin(), eg.roleCosts.end(), 0.f);
		eg.cost = 0.f;
		eg.threat = 0.f;
	}

	{
		int i = 0;
		for (const auto& kv : units) {
			int meanIndex = unitsClosestMeanID[i++];
			SEnemyGroup& eg = newMeans[meanIndex];
			CEnemyUnit* enemy = kv.second;

			// don't divide by 0
			float num = std::max(1, numUnitsAssignedToMean[meanIndex]);
			eg.pos += enemy->GetPos() / num;

			eg.units.push_back(kv.first);

			CCircuitDef* cdef = enemy->GetCircuitDef();
			if (cdef != nullptr) {
				eg.roleCosts[cdef->GetMainRole()] += cdef->GetCost();
				if (!enemy->IsHidden() && (!cdef->IsMobile() || enemy->IsInRadarOrLOS())) {
					eg.cost += cdef->GetCost();
				}
			}
			if (!enemy->IsHidden()) {
				eg.threat += enemy->GetThreat();
			}
		}
	}

	// do a check and see if there are any empty means and set the height
	enemyPos = ZeroVector;
	for (int i = 0; i < newK; i++) {
		// if a newmean is unchanged, set it to the new means pos instead of (0, 0, 0)
		if (newMeans[i].pos == ZeroVector) {
			newMeans[i] = newMeansPosition;
		} else {
			// get the proper elevation for the y-coord
//			newMeans[i].pos.y = circuit->GetMap()->GetElevationAt(newMeans[i].pos.x, newMeans[i].pos.z) + K_MEANS_ELEVATION;
		}
		enemyPos += newMeans[i].pos;
	}
	enemyPos /= newK;

//	return newMeans;
}

} // namespace circuit
