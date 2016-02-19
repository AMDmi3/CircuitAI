/*
 * GuardTask.h
 *
 *  Created on: Jan 28, 2015
 *      Author: rlcevg
 */

#ifndef SRC_CIRCUIT_TASK_FIGHTER_GUARDTASK_H_
#define SRC_CIRCUIT_TASK_FIGHTER_GUARDTASK_H_

#include "task/fighter/FighterTask.h"
#include "unit/CircuitUnit.h"

namespace circuit {

class CGuardTask: public IFighterTask {
public:
	CGuardTask(ITaskManager* mgr, CCircuitUnit* vip, float maxPower);
	virtual ~CGuardTask();

	virtual bool CanAssignTo(CCircuitUnit* unit) const;

	virtual void Execute(CCircuitUnit* unit);

	virtual void OnUnitIdle(CCircuitUnit* unit);

private:
	CCircuitUnit::Id vipId;
	float maxPower;
};

} // namespace circuit

#endif // SRC_CIRCUIT_TASK_FIGHTER_GUARDTASK_H_
