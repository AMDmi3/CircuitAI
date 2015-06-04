/*
 * AttackTask.cpp
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#include "task/fighter/AttackTask.h"
#include "task/RetreatTask.h"
#include "task/TaskManager.h"
#include "terrain/TerrainManager.h"
#include "CircuitAI.h"
#include "util/utils.h"

#include "OOAICallback.h"
#include "AISCommands.h"

namespace circuit {

using namespace springai;

CAttackTask::CAttackTask(ITaskManager* mgr) :
		IUnitTask(mgr, Priority::NORMAL, Type::ATTACK)
{
}

CAttackTask::~CAttackTask()
{
	PRINT_DEBUG("Execute: %s\n", __PRETTY_FUNCTION__);
}

void CAttackTask::Execute(CCircuitUnit* unit)
{
	CCircuitAI* circuit = manager->GetCircuit();
	Unit* u = unit->GetUnit();
	CTerrainManager* terrain = circuit->GetTerrainManager();
	int terWidth = terrain->GetTerrainWidth();
	int terHeight = terrain->GetTerrainHeight();
	float x = rand() % (int)(terWidth + 1);
	float z = rand() % (int)(terHeight + 1);
	AIFloat3 toPos(x, circuit->GetMap()->GetElevationAt(x, z), z);
	u->Fight(toPos, UNIT_COMMAND_OPTION_INTERNAL_ORDER, FRAMES_PER_SEC * 60 * 5);
}

void CAttackTask::Update()
{
	// TODO: Monitor threat? Or do it on EnemySeen/EnemyDestroyed?

	CCircuitAI* circuit = manager->GetCircuit();
	for (CCircuitUnit* unit : units) {
		CCircuitDef* cdef = unit->GetCircuitDef();
		if ((cdef->GetReloadFrames() < 0) ||
			(unit->GetDGunFrame() + cdef->GetReloadFrames() >= circuit->GetLastFrame()) ||
			unit->GetUnit()->IsParalyzed())
		{
			continue;
		}
		auto enemies = std::move(circuit->GetCallback()->GetEnemyUnitsIn(unit->GetUnit()->GetPos(), cdef->GetDGunRange() * 0.9f));
		if (!enemies.empty()) {
			for (Unit* enemy : enemies) {
				if (enemy != nullptr) {
					unit->ManualFire(enemy, circuit->GetLastFrame());
					break;
				}
			}
			utils::free_clear(enemies);
		}
	}
}

void CAttackTask::OnUnitIdle(CCircuitUnit* unit)
{
	// TODO: Wait for others if goal reached? Or we stuck far away?
	manager->AbortTask(this);
}

void CAttackTask::OnUnitDamaged(CCircuitUnit* unit, CCircuitUnit* attacker)
{
	Unit* u = unit->GetUnit();
	// TODO: floating retreat coefficient
	if (u->GetHealth() > u->GetMaxHealth() * 0.6) {
		return;
	}

	manager->AssignTask(unit, manager->GetRetreatTask());
	manager->AbortTask(this);
}

void CAttackTask::OnUnitDestroyed(CCircuitUnit* unit, CCircuitUnit* attacker)
{
//	RemoveAssignee(unit);
	manager->AbortTask(this);
}

} // namespace circuit
