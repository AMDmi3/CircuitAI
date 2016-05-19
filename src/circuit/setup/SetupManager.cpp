/*
 * SetupManager.cpp
 *
 *  Created on: Dec 9, 2014
 *      Author: rlcevg
 */

#include "setup/SetupManager.h"
#include "setup/SetupData.h"
#include "resource/MetalManager.h"
#include "terrain/TerrainManager.h"
#include "CircuitAI.h"
#include "util/Scheduler.h"
#include "util/utils.h"
#include "json/json.h"

#include "OOAICallback.h"
#include "Game.h"
#include "Map.h"
#include "DataDirs.h"
#include "File.h"
#include "Log.h"
#include "Lua.h"

#include <map>
#include <regex>

namespace circuit {

using namespace springai;

CSetupManager::CSetupManager(CCircuitAI* circuit, CSetupData* setupData)
		: circuit(circuit)
		, setupData(setupData)
		, config(nullptr)
		, commanderId(-1)
		, startPos(-RgtVector)
		, basePos(-RgtVector)
{
	const char* setupScript = circuit->GetGame()->GetSetupScript();
	if (!setupData->IsInitialized()) {
		ParseSetupScript(setupScript);
	}
	DisabledUnits(setupScript);
	circuit->GetScheduler()->RunTaskAt(std::make_shared<CGameTask>(&CSetupManager::FindCommander, this));
}

CSetupManager::~CSetupManager()
{
	PRINT_DEBUG("Execute: %s\n", __PRETTY_FUNCTION__);
	delete config;
}

void CSetupManager::ParseSetupScript(const char* setupScript)
{
	std::string script(setupScript);
	std::map<int, int> teamIdsRemap;
	using OrigTeamIds = std::set<int>;
	std::map<int, OrigTeamIds> allies;
	CSetupData::BoxMap boxes;

	// Detect start boxes
	Map* map = circuit->GetMap();
	float width = map->GetWidth() * SQUARE_SIZE;
	float height = map->GetHeight() * SQUARE_SIZE;

	std::string::const_iterator start = script.begin();
	std::string::const_iterator end = script.end();
	std::regex patternBox("startboxes=(.*);");
	std::smatch section;
	bool isZkBox = std::regex_search(start, end, section, patternBox);
	if (isZkBox) {
		// zk way
		// startboxes=return { [0] = { 0, 0, 0.25, 1 }, [1] = { 0.75, 0, 1, 1 }, };
		// @see Zero-K.sdd/LuaRules/Gadgets/start_boxes.lua
		std::string lua_str = section[1];
		start = lua_str.begin();
		end = lua_str.end();
		std::regex patternAlly("\\[(\\d+)\\][^\\{]*\\{[ ,]*(\\d+\\.?\\d*)[ ,]*(\\d+\\.?\\d*)[ ,]*(\\d+\\.?\\d*)[ ,]*(\\d+\\.?\\d*)[^\\}]\\}");
		while (std::regex_search(start, end, section, patternAlly)) {
			int allyTeamId = utils::string_to_int(section[1]);

			CAllyTeam::SBox startbox;
			startbox.left   = utils::string_to_float(section[2]) * width;
			startbox.top    = utils::string_to_float(section[3]) * height;
			startbox.right  = utils::string_to_float(section[4]) * width;
			startbox.bottom = utils::string_to_float(section[5]) * height;
			boxes[allyTeamId] = startbox;

			start = section[0].second;
		}
	} else {
		// engine way
		std::regex patternAlly("\\[allyteam(\\d+)\\]\\s*\\{([^\\}]*)\\}");
		std::regex patternRect("startrect\\w+=(\\d+(\\.\\d+)?);");
		while (std::regex_search(start, end, section, patternAlly)) {
			int allyTeamId = utils::string_to_int(section[1]);

			std::string allyBody = section[2];
			std::sregex_token_iterator iter(allyBody.begin(), allyBody.end(), patternRect, 1);
			std::sregex_token_iterator end;
			CAllyTeam::SBox startbox;
			for (int i = 0; iter != end && i < 4; ++iter, i++) {
				startbox.edge[i] = utils::string_to_float(*iter);
			}

			startbox.bottom *= height;
			startbox.left   *= width;
			startbox.right  *= width;
			startbox.top    *= height;
			boxes[allyTeamId] = startbox;

			start = section[0].second;
		}
	}

	// Detect start position type
	CGameSetup::StartPosType startPosType;
	std::cmatch matchPosType;
	std::regex patternPosType("startpostype=(\\d+)");
	if (std::regex_search(setupScript, matchPosType, patternPosType)) {
		startPosType = static_cast<CGameSetup::StartPosType>(std::atoi(matchPosType[1].first));
	} else {
		startPosType = CGameSetup::StartPosType::StartPos_Fixed;
	}

	// Count number of alliances
	std::regex patternAlly("\\[allyteam(\\d+)\\]");
	start = script.begin();
	end = script.end();
	while (std::regex_search(start, end, section, patternAlly)) {
		int allyTeamId = utils::string_to_int(section[1]);
		allies[allyTeamId];  // create empty alliance
		start = section[0].second;
	}

	// Detect team alliances
	std::regex patternTeam("\\[team(\\d+)\\]\\s*\\{([^\\}]*)\\}");
	std::regex patternAllyId("allyteam=(\\d+);");
	start = script.begin();
	end = script.end();
	while (std::regex_search(start, end, section, patternTeam)) {
		int teamId = utils::string_to_int(section[1]);
		teamIdsRemap[teamId] = teamId;

		std::string teamBody = section[2];
		std::smatch matchAllyId;
		if (std::regex_search(teamBody, matchAllyId, patternAllyId)) {
			int allyTeamId = utils::string_to_int(matchAllyId[1]);
			allies[allyTeamId].insert(teamId);
		}

		start = section[0].second;
	}
	// Make team remapper
	int i = 0;
	for (auto& kv : teamIdsRemap) {
		kv.second = i++;
	}

	// Remap teams, create ally-teams
	// @see rts/Game/GameSetup.cpp CGameSetup::Init
	CSetupData::AllyMap allyTeams;
	allyTeams.reserve(allies.size());
	for (const auto& kv : allies) {
		const OrigTeamIds& data = kv.second;

		CAllyTeam::TeamIds teamIds;
		teamIds.reserve(data.size());
		for (auto id : data) {
			teamIds.insert(teamIdsRemap[id]);
		}
		allyTeams.push_back(new CAllyTeam(teamIds, isZkBox ? boxes[0] : boxes[kv.first]));
	}

	setupData->Init(allyTeams, boxes, startPosType);
}

void CSetupManager::DisabledUnits(const char* setupScript)
{
	std::string script(setupScript);
	std::regex patternDisabled("disabledunits=(.*);");
	std::string::const_iterator start = script.begin();
	std::string::const_iterator end = script.end();
	std::smatch section;
	if (std::regex_search(start, end, section, patternDisabled)) {
		// !setoptions disabledunits=raveparty+zenith+mahlaze
		std::string opt_str = section[1];
		start = opt_str.begin();
		end = opt_str.end();
		std::regex patternUnit("\\w+");
		while (std::regex_search(start, end, section, patternUnit)) {
			CCircuitDef* cdef = circuit->GetCircuitDef(std::string(section[0]).c_str());
			if (cdef != nullptr) {
				cdef->SetMaxThisUnit(0);
			}
			start = section[0].second;
		}
	}
}

bool CSetupManager::OpenConfig(const std::string& cfgName)
{
	if (cfgName.empty()) {
		/*
		 * Locate map specific config
		 */
		Map* map = circuit->GetMap();
		std::string filename = "LuaRules/Configs/CircuitAI/";
		configName = utils::MakeFileSystemCompatible(map->GetName()) + ".json";
		filename += configName;

		const char* cfgJson = ReadConfig(filename);
		if (cfgJson != nullptr) {
			config = ParseConfig(cfgJson);
			delete[] cfgJson;

			if (config != nullptr) {
				return true;
			}
		}
	}

	std::string cfgDefault("circuit");
	{
		/*
		 * Locate game specific config
		 */
		std::string filename = "LuaRules/Configs/CircuitAI/";
		configName = cfgDefault + ".json";
		filename += configName;

		const char* cfgJson = ReadConfig(filename);
		if (cfgJson != nullptr) {
			config = ParseConfig(cfgJson);
			delete[] cfgJson;

			if (config != nullptr) {
				return true;
			}
		}
	}
	if (!cfgName.empty()) {
		cfgDefault = cfgName;
	}

	/*
	 * Locate global config
	 */
	std::string filename("config" SLASH);
	configName = (cfgDefault.find(".json") == std::string::npos) ? (cfgDefault + ".json") : cfgDefault;
	filename += configName;
	if (!LocatePath(filename)){
		circuit->LOG("Config file is missing! (%s)", configName.c_str());
		return false;
	}

	// read config
	const char* cfgJson = ReadConfig(filename);
	if (cfgJson == nullptr) {
		return false;
	}

	// parse config
	config = ParseConfig(cfgJson);
	delete[] cfgJson;

	return (config != nullptr);
}

void CSetupManager::CloseConfig()
{
	delete config;
	config = nullptr;
}

bool CSetupManager::HasStartBoxes() const
{
	return setupData->IsInitialized();
}

bool CSetupManager::CanChooseStartPos() const
{
	return setupData->CanChooseStartPos();
}

void CSetupManager::PickStartPos(CCircuitAI* circuit, StartPosType type)
{
	float x, z;
	const CAllyTeam::SBox& box = circuit->GetAllyTeam()->GetStartBox();

	auto random = [](const CAllyTeam::SBox& box, float& x, float& z) {
		int min, max;
		min = box.left;
		max = box.right;
		x = min + (rand() % (int)(max - min + 1));
		min = box.top;
		max = box.bottom;
		z = min + (rand() % (int)(max - min + 1));
	};

	switch (type) {
		case StartPosType::METAL_SPOT: {
			const CMetalData::Metals& spots = circuit->GetMetalManager()->GetSpots();
			CMetalData::MetalIndices indices;
			indices.reserve(spots.size());
			for (unsigned idx = 0; idx < spots.size(); ++idx) {
				indices.push_back(idx);
			}
			std::random_shuffle(indices.begin(), indices.end());

			CTerrainManager* terrainManager = circuit->GetTerrainManager();
			STerrainMapMobileType* mobileType = terrainManager->GetMobileTypeById(circuit->GetCircuitDef("armcom1")->GetMobileId());
			Lua* lua = circuit->GetCallback()->GetLua();
			bool isDone = false;

			for (unsigned idx : indices) {
				std::string cmd("ai_is_valid_startpos:");
				const CMetalData::SMetal& spot = spots[idx];
				cmd += utils::int_to_string(spot.position.x) + "/" + utils::int_to_string(spot.position.z);
				std::string result = lua->CallRules(cmd.c_str(), cmd.size());
				if (result != "1") {
					continue;
				}

				int iS = terrainManager->GetSectorIndex(spots[idx].position);
				STerrainMapArea* area = mobileType->sector[iS].area;
				if ((area != nullptr) && area->areaUsable) {
					x = spot.position.x;
					z = spot.position.z;
					isDone = true;
					break;
				}
			}

			delete lua;
			if (isDone) {
				break;
			}

//			AIFloat3 posFrom(box.left, 0, box.top);
//			AIFloat3 posTo(box.right, 0, box.bottom);
//			CMetalManager* metalManager = circuit->GetMetalManager();
//			CMetalData::MetalIndices inBoxIndices = metalManager->FindWithinRangeSpots(posFrom, posTo);
//			if (!inBoxIndices.empty()) {
//				const CMetalData::Metals& spots = metalManager->GetSpots();
//				CTerrainManager* terrainManager = circuit->GetTerrainManager();
//				STerrainMapMobileType* mobileType = terrainManager->GetMobileTypeById(circuit->GetCircuitDef("armcom1")->GetMobileId());
//				std::vector<int> filteredIndices;
//				for (auto idx : inBoxIndices) {
//					int iS = terrainManager->GetSectorIndex(spots[idx].position);
//					STerrainMapArea* area = mobileType->sector[iS].area;
//					if ((area != nullptr) && area->areaUsable) {
//						filteredIndices.push_back(idx);
//					}
//				}
//				if (!filteredIndices.empty()) {
//					const AIFloat3& pos = spots[filteredIndices[rand() % filteredIndices.size()]].position;
//					x = pos.x;
//					z = pos.z;
//					break;
//				}
//			}

			random(box, x, z);
			break;
		}
		case StartPosType::MIDDLE: {
			x = (box.left + box.right) / 2;
			z = (box.top + box.bottom) / 2;
			break;
		}
		case StartPosType::RANDOM:
		default: {
			random(box, x, z);
			break;
		}
	}

	AIFloat3 pos = AIFloat3(x, circuit->GetMap()->GetElevationAt(x, z), z);
	SetStartPos(pos);
	circuit->GetGame()->SendStartPosition(false, pos);
}

void CSetupManager::PickCommander()
{
	/*
	 * dyntrainer_recon_base
	 * dyntrainer_support_base
	 * dyntrainer_assault_base
	 * dyntrainer_strike_base
	 */
	std::vector<CCircuitDef*> commanders;
	CCircuitDef* supCom = nullptr;
	float bestPower = .0f;

	const CCircuitAI::CircuitDefs& defs = circuit->GetCircuitDefs();
	for (auto& kv : defs) {
		CCircuitDef* cdef = kv.second;

		std::string lvl1 = cdef->GetUnitDef()->GetName();
		if ((lvl1.find("dyntrainer_") != 0) || (lvl1.find("_base") != lvl1.size() - 5)) {
			continue;
		}

		const std::map<std::string, std::string>& customParams = cdef->GetUnitDef()->GetCustomParams();
		auto it = customParams.find("level");
		if ((it == customParams.end()) || (utils::string_to_int(it->second) != 1)) {
			continue;
		}
		commanders.push_back(cdef);

		if (bestPower < cdef->GetBuildDistance()) {  // No more UnitDef->GetAutoHeal() :(
			bestPower = cdef->GetBuildDistance();
			supCom = cdef;
		}
	}
	if (commanders.empty()) {
		return;
	}

	std::string cmd("ai_commander:");
	cmd += ((supCom == nullptr) ? commanders[rand() % commanders.size()] : supCom)->GetUnitDef()->GetName();
	Lua* lua = circuit->GetCallback()->GetLua();
	lua->CallRules(cmd.c_str(), cmd.size());
	delete lua;
}

CCircuitUnit* CSetupManager::GetCommander() const
{
	return circuit->GetTeamUnit(commanderId);
}

CAllyTeam* CSetupManager::GetAllyTeam() const
{
	return setupData->GetAllyTeam(circuit->GetAllyTeamId());
}

void CSetupManager::FindCommander()
{
	std::vector<Unit*> units = circuit->GetCallback()->GetTeamUnits();
	for (Unit* u : units) {
		UnitDef* def = u->GetDef();
		bool valid = def->IsBuilder();
		delete def;
		if (valid) {
			commanderId = u->GetUnitId();
//			if (!utils::is_valid(startPos)) {
				SetStartPos(u->GetPos());
//			}
			break;
		}
	}
	utils::free_clear(units);
}

bool CSetupManager::LocatePath(std::string& filename)
{
	static const size_t absPath_sizeMax = 2048;
	char absPath[absPath_sizeMax];
	DataDirs* datadirs = circuit->GetCallback()->GetDataDirs();
	const bool dir = !filename.empty() && (*filename.rbegin() == '/' || *filename.rbegin() == '\\');
	const bool located = datadirs->LocatePath(absPath, absPath_sizeMax, filename.c_str(), false /*writable*/, false /*create*/, dir, false /*common*/);
	if (located) {
		filename = absPath;
	}
	delete datadirs;
	return located;
}

const char* CSetupManager::ReadConfig(const std::string& filename)
{
	File* file = circuit->GetCallback()->GetFile();
	int fileSize = file->GetSize(filename.c_str());
	if (fileSize <= 0) {
		circuit->LOG("Malformed config file! (%s)", configName.c_str());
		delete file;
		return nullptr;
	}

	char* cfgJson = new char [fileSize + 1];
	file->GetContent(filename.c_str(), cfgJson, fileSize);
	cfgJson[fileSize] = 0;
	delete file;
	return cfgJson;
}

Json::Value* CSetupManager::ParseConfig(const char* cfgJson)
{
	Json::Value jsonAll;
	Json::Reader json;
	bool isOk = json.parse(cfgJson, jsonAll, false);
	if (!isOk) {
		circuit->LOG("Malformed config format! (%s)", configName.c_str());
		return nullptr;
	}

	const char* diffs[] = {setup::easy, setup::normal, setup::hard};
	const char* diffName = diffs[static_cast<size_t>(circuit->GetDifficulty())];
	Json::Value& jsonDiff = jsonAll[diffName];
	if (jsonDiff == Json::Value::null) {
		circuit->LOG("Malformed difficulty! (%s : %s)", configName.c_str(), diffName);
		const char* diffDefault = jsonAll.get("default", "normal").asCString();
		jsonDiff = jsonAll[diffDefault];
		if (jsonDiff == Json::Value::null) {
			circuit->LOG("Malformed difficulty! (%s : %s)", configName.c_str(), diffDefault);
			return nullptr;
		}
	}

	Json::Value* cfg = new Json::Value;
	*cfg = jsonDiff;
	return cfg;
}

} // namespace circuit
