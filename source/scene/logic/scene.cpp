﻿#include "scene.h"
#include "sceneMgr.h"
Scene::Scene(SceneID id)
{
    _sceneID = id;
    cleanScene();
}
Scene::~Scene()
{
    if(_sim)
    {
        delete _sim;
        _sim = nullptr;
    }
}
GroupID Scene::getGroupID(ServiceID avatarID)
{
    auto entity = getEntityByAvatarID(avatarID);
    if (entity)
    {
        return entity->_entityInfo.groupID;
    }
    return InvalidGroupID;
}
void Scene::getSceneSection(SceneSection & ss)
{
    ss.sceneID = _sceneID;
    ss.sceneType = _sceneType;
    ss.sceneState = _sceneStatus;
    ss.sceneStartTime = _startTime;
    ss.sceneEndTime = _endTime;
    ss.serverTime = getFloatSteadyNowTime();
    for (auto & entity : _entitys)
    {
        ss.entitys.push_back(entity.second->getFullData());
    }
}



bool Scene::cleanScene()
{
    _lastEID = ServerConfig::getRef().getSceneConfig()._lineID * 1000 + 1000;
    _entitys.clear();
    _players.clear();
    _monsters.clear();
    while (!_asyncs.empty()) _asyncs.pop();
    _sceneType = SCENE_NONE;
    _sceneStatus = SCENE_STATE_NONE;
    _lastStatusChangeTime = getFloatSteadyNowTime();
    _lastPrintStatus = _lastStatusChangeTime;
    _lastDoRVO = _lastStatusChangeTime;
    if (_sim)
    {
        delete _sim;
        _sim = nullptr;
    }

    return true;
}

bool Scene::initScene(SceneType sceneType, MapID mapID)
{
    if (_sceneStatus != SCENE_STATE_NONE || _sim)
    {
        LOGE("Scene::loadScene  scene status error");
        return false;
    }
    _sceneType = sceneType;
    _sceneStatus = SCENE_STATE_ACTIVE;
    _lastStatusChangeTime = getFloatSteadyNowTime();
    _startTime = getFloatSteadyNowTime();
    _endTime = getFloatSteadyNowTime() + 600;
    _sim = new RVO::RVOSimulator();
    _sim->setTimeStep(SceneFrameInterval);
    _sim->setAgentDefaults(15.0, 1000, 70.0, 70.0, 2.0, 7.0);
    if (false)
    {
        std::vector<RVO::Vector2> vertices;
        vertices.push_back(RVO::Vector2(-7.0, -20.0));
        vertices.push_back(RVO::Vector2(7.0, -20.0));
        vertices.push_back(RVO::Vector2(7.0, 20.0));
        vertices.push_back(RVO::Vector2(-7.0, 20.0));
        _sim->addObstacle(vertices);
        _sim->processObstacles();
    }

    //load map
    //load entitys

    return true;
}



EntityPtr Scene::getEntity(EntityID eID)
{
    auto founder = _entitys.find(eID);
    if (founder == _entitys.end())
    {
        return nullptr;
    }
    return founder->second;
}
EntityPtr Scene::getEntityByAvatarID(ServiceID avatarID)
{
    auto founder = _players.find(avatarID);
    if (founder == _players.end())
    {
        return nullptr;
    }
    return founder->second;
}



EntityPtr Scene::addEntity(const EntityBase & baseInfo,
    const EntityProp & fixedProps,
    const EntityProp & growProps,
    const EntityProp & growth,
    GroupID groupID)
{
    EntityPtr entity = std::make_shared<Entity>();

    entity->_baseInfo = baseInfo;
    entity->_fixedProps = fixedProps;
    entity->_growProps = growProps;
    entity->_growth = growth;

    entity->_props = processPropGrow(fixedProps, growProps, growth, baseInfo.level);
    

    entity->_entityInfo.eid = ++_lastEID;
    entity->_entityInfo.camp = baseInfo.camp;
    entity->_entityInfo.etype = baseInfo.etype;
    entity->_entityInfo.groupID = groupID;
    entity->_entityInfo.state = baseInfo.state;
    entity->_entityInfo.leader = InvalidEntityID;
    entity->_entityInfo.foe = InvalidEntityID;

    entity->_entityInfo.curHP = entity->_props.hp;

    entity->_control.spawnpoint = { 0.0 - 30 +  realRandF()*30 ,60 -30 + realRandF()*30 };
    entity->_control.eid = entity->_entityInfo.eid;
    entity->_control.agentNo = -1;
    entity->_control.stateChageTime = getFloatSteadyNowTime();

    entity->_entityMove.eid = entity->_entityInfo.eid;
    entity->_entityMove.position = entity->_control.spawnpoint;
    entity->_entityMove.follow = InvalidEntityID;
    entity->_entityMove.waypoints.clear();
    entity->_entityMove.action = MOVE_ACTION_IDLE;

    entity->_report.eid = entity->_entityInfo.eid;

    entity->_control.agentNo = _sim->addAgent(toRVOVector2(entity->_entityMove.position));
    _entitys.insert(std::make_pair(entity->_entityInfo.eid, entity));

    if (baseInfo.avatarID != InvalidServiceID && entity->_entityInfo.etype == ENTITY_PLAYER)
    {
        _players[baseInfo.avatarID] = entity;
        _sim->setAgentRadius(entity->_control.agentNo, 0.5f);
    }
    else
    {
        _sim->setAgentRadius(entity->_control.agentNo, 1.0f);
    }

    AddEntityNotice notice;
    notice.entitys.push_back(entity->getFullData());
    broadcast(notice, entity->_baseInfo.avatarID);

    return entity;
}
bool Scene::removePlayer(AvatarID avatarID)
{
    auto entity = getEntityByAvatarID(avatarID);
    if (entity)
    {
        return removeEntity(entity->_entityInfo.eid);
    }
    return false;
}
bool Scene::removePlayerByGroupID(GroupID groupID)
{
    std::set<EntityID> removes;
    for (auto entity : _entitys)
    {
        if (entity.second->_entityInfo.etype == ENTITY_PLAYER && entity.second->_entityInfo.groupID == groupID)
        {
            removes.insert(entity.second->_entityInfo.eid);
        }
    }
    for (auto eid : removes)
    {
        removeEntity(eid);
    }
    return true;
}


bool Scene::removeEntity(EntityID eid)
{
    auto entity = getEntity(eid);
    if (!entity)
    {
        LOGE("");
        return false;
    }
    if(entity->_control.agentNo < _sim->getNumAgents())
    {
        _sim->removeAgent(entity->_control.agentNo);
        entity->_control.agentNo = -1;
    }
    if (entity->_entityInfo.etype == ENTITY_PLAYER)
    {
        _players.erase(entity->_baseInfo.avatarID);
        SceneMgr::getRef().sendToWorld(SceneServerGroupStateChangeIns(getSceneID(), entity->_entityInfo.groupID, SCENE_NONE));
    }
    _entitys.erase(eid);

    RemoveEntityNotice notice;
    notice.eids.push_back(eid);
    broadcast(notice);
    return true;
}

bool Scene::playerAttach(ServiceID avatarID, SessionID sID)
{

    EntityPtr entity = getEntityByAvatarID(avatarID);
    if (!entity)
    {
        return false;
    }
    entity->_clientSessionID = sID;

    LOGI("Scene::playerAttach avatarName=" << entity->_baseInfo.avatarName << " sessionID=" << sID << ", entityID=" << entity->_entityInfo.eid);
    EntityFullDataArray entitys;
    SceneSectionNotice section;
    getSceneSection(section.section);
    entitys.swap(section.section.entitys);
    sendToClient(avatarID, section);

    AddEntityNotice notice;
    while (!entitys.empty())
    {
        notice.entitys.push_back(entitys.back());
        entitys.pop_back();
        if (notice.entitys.size() >= 10)
        {
            sendToClient(avatarID, notice);
            notice.entitys.clear();
        }
    }
    if (!notice.entitys.empty())
    {
        sendToClient(avatarID, notice);
    }
    return true;
}



bool Scene::playerDettach(ServiceID avatarID, SessionID sID)
{
    auto entity = getEntityByAvatarID(avatarID);
    if (entity && entity->_clientSessionID == sID)
    {
        LOGI("Scene::playerDettach avatarName=" << entity->_baseInfo.avatarName << " sessionID=" << sID << ", entityID=" << entity->_entityInfo.eid);
        entity->_clientSessionID = InvalidSessionID;
    }
    return true;
}



bool Scene::onUpdate()
{
    if (getFloatSteadyNowTime() > _endTime)
    {
        return false;
    }
    if (getFloatSteadyNowTime() - _lastCheckMonstr > 0.5)
    {
        _lastCheckMonstr = getFloatSteadyNowTime();
        doMonster();
        doFollow();
    }
    if (getFloatSteadyNowTime() - _lastPrintStatus > 10)
    {
        _lastPrintStatus = getFloatSteadyNowTime();
        LOGI("sceneID=" << _sceneID << ", rvo sum second=" << _sim->getGlobalTime() << ", scene sum second=" << getFloatSteadyNowTime() - _startTime);
    }
    doStepRVO();
    checkSkillBehaviour();
    checkSceneState();



    SceneRefreshNotice notice;
    for (auto &kv : _entitys)
    {
        if (kv.second->_isInfoDirty)
        {
            notice.entityInfos.push_back(kv.second->_entityInfo);
            kv.second->_isInfoDirty = false;
        }
        if (kv.second->_isMoveDirty)
        {
            notice.entityMoves.push_back(kv.second->_entityMove);
            kv.second->_isMoveDirty = false;
        }
    }
    if (!notice.entityInfos.empty() || !notice.entityMoves.empty())
    {
        broadcast(notice);
    }
    while (!_asyncs.empty())
    {
        auto func = _asyncs.front();
        _asyncs.pop();
        func();
    }

    return true;
}

void Scene::checkStepRVO(bool preCheck)
{
    if (_sim)
    {
        for (auto &kv:_entitys)
        {
            auto &entity = *kv.second;
            if (entity._control.agentNo  >= _sim->getNumAgents())
            {
                continue;
            }
            if (entity._entityMove.action == MOVE_ACTION_IDLE)
            {
                continue;
            }
            do
            {
                while (!entity._entityMove.waypoints.empty())
                {
                    double dist = getDistance(entity._entityMove.position, entity._entityMove.waypoints.front());
                    if (dist < 1.0 || (dist < 6.0&&entity._entityMove.action == MOVE_ACTION_FOLLOW))
                    {
                        entity._entityMove.waypoints.erase(entity._entityMove.waypoints.begin());
                        continue;
                    }
                    break;
                }
                if (entity._entityMove.waypoints.empty())
                {
                    LOGD("END MOVE[" << entity._baseInfo.avatarName << "]: all waypoints is gone");
                    entity._entityMove.action = MOVE_ACTION_IDLE;
                    break;
                }
                if (entity._control.blockMoveCount > 1.0/SceneFrameInterval)
                {
                    LOGW("BREAK MOVE[" << entity._baseInfo.avatarName << "][" << entity._entityInfo.eid << "]: block long time. count = " << entity._control.blockMoveCount);
                    entity._entityMove.waypoints.clear();
                    entity._entityMove.action = MOVE_ACTION_IDLE;
                    entity._control.blockMoveCount = 0;
                    break;
                }
                if (!preCheck)
                {
                    break;
                }
                if (::accessFile("../rvo.txt"))
                {
                    std::string content = readFileContent("../rvo.txt");
                    auto tp = splitTupleString<double, size_t, double, double, double>(content, ",", " ");
                    _sim->setAgentNeighborDist(entity._control.agentNo, std::get<0>(tp));
                    _sim->setAgentMaxNeighbors(entity._control.agentNo, std::get<1>(tp));
                    _sim->setAgentTimeHorizon(entity._control.agentNo, std::get<2>(tp));
                    _sim->setAgentTimeHorizonObst(entity._control.agentNo, std::get<3>(tp));
                    _sim->setAgentRadius(entity._control.agentNo, std::get<4>(tp));
                }
                
                _sim->setAgentMaxSpeed(entity._control.agentNo, entity._entityMove.expectSpeed);
                double dist = getDistance(entity._entityMove.position, entity._entityMove.waypoints.front());
                double needTime = dist / entity._entityMove.expectSpeed;
                RVO::Vector2 dir = RVO::normalize(toRVOVector2(entity._entityMove.waypoints.front()) - toRVOVector2(entity._entityMove.position));
                if (needTime > SceneFrameInterval)
                {
                    dir *= entity._entityMove.expectSpeed;
                }
                else
                {
                    dir *= needTime / SceneFrameInterval;
                }
                _sim->setAgentPrefVelocity(entity._control.agentNo, dir);
                LOGD("RVO PRE MOVE[" << entity._baseInfo.avatarName << "] local=" << entity._entityMove.position
                     << ", dst=" << entity._entityMove.waypoints.front() << ", dir=" << dir);
            } while (false);

            if (entity._entityMove.action == MOVE_ACTION_IDLE)
            {
                entity._entityMove.waypoints.clear();
                _sim->setAgentPrefVelocity(entity._control.agentNo, RVO::Vector2(0, 0));
                broadcast(MoveNotice(entity._entityMove));
                LOGD("RVO FIN MOVE[" << entity._baseInfo.avatarName << "] local=" << entity._entityMove.position);
            }
            entity._isMoveDirty = true;
        }
    }
}
void Scene::doStepRVO()
{
    checkStepRVO(true);
    if (_sim)
    {
        double timeStep = getFloatSteadyNowTime() - _lastDoRVO;
        _lastDoRVO = getFloatSteadyNowTime();
        _sim->setTimeStep(timeStep);
        _sim->doStep();
        for (auto &kv : _entitys)
        {
            auto &entity = *kv.second;
            if (entity._control.agentNo >= _sim->getNumAgents())
            {
                continue;
            }
            auto rvoPos = toEPoint(_sim->getAgentPosition(entity._control.agentNo));
            if (getDistance(entity._entityMove.position, rvoPos) > 0.1)
            {
                entity._isMoveDirty = true;
            }
            if (entity._isMoveDirty)
            {
                auto realMove = toRVOVector2(rvoPos) - toRVOVector2(entity._entityMove.position);
                auto expectMove = _sim->getAgentPrefVelocity(entity._control.agentNo);
                entity._entityMove.realSpeed = RVO::abs(realMove) / timeStep;
                if (RVO::abs(expectMove) > 0.0001) //float over
                {
                    if (RVO::abs(realMove) / (RVO::abs(expectMove)/ServerPulseInterval) < 0.1)
                    {
                        entity._control.blockMoveCount++;
                        LOGW("EXPECT MOVE DIST WRONG[" << entity._entityInfo.eid << "]: DIFF=" << RVO::abs(realMove) / RVO::abs(expectMove) << ", now blocks=" << entity._control.blockMoveCount);
                    }
                    else
                    {
                        entity._control.blockMoveCount = 0;
                    }
                }
            }

            entity._entityMove.position = toEPoint(_sim->getAgentPosition(entity._control.agentNo));
            if (entity._isMoveDirty)
            {
                LOGD("RVO AFT MOVE[" << entity._baseInfo.avatarName << "] local=" << entity._entityMove.position);
            }
            
        }
    }
    checkStepRVO(false);
}


void Scene::doMonster()
{
    if (_sceneType != SCENE_HOME)
    {
        return;
    }
    while (_monsters.size() < _players.size() * 3  )
    {
        EntityBase base;
        base.avatarID = 1000 + _monsters.size();
        base.avatarName = "MyLittlePet_";
        base.avatarName += toString(_monsters.size());
        base.modelID = rand()%45+1;
        base.modelName = base.avatarName;
        base.camp = ENTITY_CAMP_BLUE + 100;
        base.etype = ENTITY_AI;
        base.state = ENTITY_STATE_ACTIVE;
        EntityProp fixedProps;
        fixedProps.hp = 1000;
        fixedProps.attack = 10;
        EntityProp grow;

        auto monster = addEntity(base, fixedProps, grow, grow);
        _monsters[monster->_entityInfo.eid] = monster;
    }
    for (auto monster: _monsters)
    {
        SearchInfo search;
        search.camp = SEARCH_CAMP_NONE;
        search.etype = ENTITY_PLAYER;
        search.limitEntitys = 1;
        search.radian = PI*2.0;
        search.method = SEARCH_METHOD_SEACTOR;
        search.distance = 1E20;
        search.offsetX = 0;
        search.offsetY = 0;
        auto ret = searchTarget(monster.second, 0, search);
        if (ret.size() > 0)
        {
            if (monster.second->_entityMove.follow != ret.front()->_entityInfo.eid && monster.second->_entityInfo.state == ENTITY_STATE_ACTIVE)
            {
                if (monster.second->_entityMove.follow == InvalidEntityID || realRand() > 0.7)
                {
                    monster.second->_entityMove.follow = ret.front()->_entityInfo.eid;
                }
            }
        }
    }
}

void Scene::doFollow()
{
    for (auto kv: _entitys)
    {
        if (kv.second->_entityMove.follow == InvalidEntityID)
        {
            continue;
        }
        auto entity = kv.second;
        auto follow = getEntity(kv.second->_entityMove.follow);
        if (!follow)
        {
            kv.second->_entityMove.follow = InvalidEntityID;
            continue;
        }
        auto dist = getDistance(entity->_entityMove.position, follow->_entityMove.position);
        if (dist >  12) 
        {
            EPositionArray ways;
            ways.push_back(follow->_entityMove.position);
            doMove(entity->_entityInfo.eid, MOVE_ACTION_FOLLOW, entity->getSpeed(), follow->_entityInfo.eid,
                   entity->_entityMove.position, ways);
        }
    }
}

void Scene::pushAsync(std::function<void()> && func)
{
    _asyncs.push(std::move(func));
}

void Scene::onPlayerInstruction(ServiceID avatarID, ReadStream & rs)
{
    if (avatarID == InvalidAvatarID)
    {
        return;
    }
    if (rs.getProtoID() == MoveReq::getProtoID())
    {
        MoveReq req;
        rs >> req;
        LOGD("MoveReq avatarID[" << avatarID << "] req=" << req);
        auto entity = getEntity(req.eid);
        if (!entity || entity->_baseInfo.avatarID != avatarID || entity->_entityInfo.etype != ENTITY_PLAYER 
            || req.action == MOVE_ACTION_PASV_PATH || req.action == MOVE_ACTION_FORCE_PATH 
                    || !doMove(req.eid, (MoveAction)req.action, entity->getSpeed(), req.follow, req.clientPos, req.waypoints))
        {
            sendToClient(avatarID, MoveResp(EC_ERROR, req.eid, req.action));
        }
    }
    else if (rs.getProtoID() == UseSkillReq::getProtoID())
    {
        UseSkillReq req;
        rs >> req;
        auto entity = getEntity(req.eid);
        if (!entity || entity->_baseInfo.avatarID != avatarID || entity->_entityInfo.etype != ENTITY_PLAYER
            ||!doSkill(req.eid, req.skillID, req.foe, req.dst))
        {
            sendToClient(avatarID, UseSkillResp(EC_ERROR, req.eid, req.skillID, req.foe, req.dst));
        }
    }
    else if (rs.getProtoID() == ClientCustomReq::getProtoID())
    {
        ClientCustomReq req;
        rs >> req;
        auto entity = getEntity(req.eid);
        if (entity && entity->_baseInfo.avatarID == avatarID)
        {
            broadcast(ClientCustomNotice(req.eid, req.customID, req.fValue,req.uValue, req.sValue));
        }
        else
        {
            sendToClient(avatarID, ClientCustomResp(EC_ERROR, req.eid, req.customID));
        }
    }
    else if (rs.getProtoID() == ClientPingTestReq::getProtoID())
    {
        ClientPingTestReq req;
        rs >> req;
        sendToClient(avatarID, ClientPingTestResp(EC_ERROR, req.seqID, req.clientTime));
    }
}

bool Scene::doMove(ui64 eid, MoveAction action, double speed, ui64 follow, EPosition clt, EPositionArray dsts)
{
    auto entity = getEntity(eid);
    if(!entity)
    {
        return false;
    }
    if (entity->_control.agentNo >= _sim->getNumAgents())
    {
        return false;
    }
    if (entity->_entityInfo.state != ENTITY_STATE_ACTIVE)
    {
        return false;
    }
    auto & moveInfo = entity->_entityMove;
    if (moveInfo.action == MOVE_ACTION_PASV_PATH || moveInfo.action == MOVE_ACTION_FORCE_PATH)
    {
        return false;
    }
    //stop
    if (action == MOVE_ACTION_IDLE)
    {
        moveInfo.action = MOVE_ACTION_IDLE;
        moveInfo.realSpeed = 0.0;
        moveInfo.expectSpeed; //don't reset here
        moveInfo.follow; //don't reset here
        moveInfo.waypoints.clear();
        _sim->setAgentPrefVelocity(entity->_control.agentNo, RVO::Vector2(0, 0));
    }
    //begin move
    else if (moveInfo.action == MOVE_ACTION_IDLE)
    {
        moveInfo.action = action;
        moveInfo.realSpeed = 0.0f;
        moveInfo.expectSpeed = speed;
        moveInfo.follow = follow;
        moveInfo.waypoints = dsts;
    }
    //refresh move
    else
    {
        moveInfo.action = action;
        moveInfo.realSpeed;
        moveInfo.expectSpeed = speed;
        moveInfo.follow = follow;
        moveInfo.waypoints = dsts;
    }
    entity->_isMoveDirty = true;
    broadcast(MoveNotice(moveInfo));
    return true;
}
bool Scene::doSkill(EntityID eid, ui64 skillID, EntityID foe, const EPosition & dst)
{
    auto entity = getEntity(eid);
    if (!entity)
    {
        return false;
    }
    if (entity->_entityInfo.state != ENTITY_STATE_ACTIVE)
    {
        return false;
    }
    auto &self = *entity;

    SkillData skill;
    skill.cd = 0;
    skill.skillID = skillID;
    skill.skillType = setBitFlag(0, SKILL_AUTO);

    SkillBehaviour behaviour;
    behaviour.behaviour = setBitFlag(0, SKILL_BEHAVIOUR_HIT);
    behaviour.delay = 0.15;
    behaviour.search.offsetX = -1;
    behaviour.search.offsetY = -1;
    behaviour.search.radian = PI/2.0;
    behaviour.search.camp = setBitFlag(0, SEARCH_CAMP_ALIEN);
    behaviour.search.distance = 8.0;
    behaviour.search.limitEntitys = 100;
    skill.behaviours.push_back(behaviour);

    SkillInfo info;
    info.data = skill;
    info.dst = dst;
    info.foe = foe;
    info.skillID = skillID;
    info.startTime = getFloatSteadyNowTime();

    auto founder = std::find_if(self._control.skills.begin(), self._control.skills.end(),
                                [&info](const SkillInfo & skill){return skill.skillID == info.skillID;});
    if (founder != self._control.skills.end() && getFloatNowTime() - founder->startTime > founder->data.cd
        &&founder->data.behaviours.empty())
    {
        self._control.skills.erase(founder);
    }
    else if (founder != self._control.skills.end())
    {
        return false;//in cd
    }

    self._control.skills.push_back(info);
    broadcast(UseSkillNotice(eid, skillID, foe, dst));
    return true;
}

bool Scene::checkSkillBehaviour()
{
    double now = getFloatNowTime();
    for (auto kv:_entitys)
    {
        EntityPtr self = kv.second;
        if (self->_control.skills.empty())
        {
            continue;
        }
        for (auto &skill : self->_control.skills)
        {
            while (!skill.data.behaviours.empty())
            {
                auto first = skill.data.behaviours.front();
                if (now < first.delay + skill.startTime)
                {
                    break;
                }
                if (getBitFlag(first.behaviour, SKILL_BEHAVIOUR_HIT))
                {
                    auto targets = searchTarget(self, getRadian(self->_entityMove.position.x, self->_entityMove.position.y, skill.dst.x, skill.dst.y), first.search);
                    attackTargets(self, targets);
                }
                skill.data.behaviours.erase(skill.data.behaviours.begin());
            }
        }
    }


    return true;
}

bool Scene::attackTargets(EntityPtr caster, std::vector<EntityPtr> & targets)
{
    EntityPtr master = caster;
    if (caster->_entityInfo.etype == ENTITY_FLIGHT)
    {
        master = getEntity(caster->_entityInfo.foe);
        if (!master)
        {
            return false;
        }
    }
    SceneEventNotice notice;
    for (auto target : targets)
    {
        if (target->_entityInfo.state != ENTITY_STATE_ACTIVE)
        {
            continue;
        }
        target->_entityInfo.curHP -= 20;
        target->_isInfoDirty = true;
        notice.info.push_back(SceneEventInfo(master->_entityInfo.eid, target->_entityInfo.eid, SCENE_EVENT_HARM_ATTACK, 20, ""));
        if (target->_entityInfo.curHP <= 0)
        {
            target->_entityInfo.curHP = 0.0;
            target->_entityInfo.state = ENTITY_STATE_LIE;
            target->_entityMove.action = MOVE_ACTION_IDLE;
            target->_entityMove.follow = InvalidEntityID;
            target->_entityInfo.foe = InvalidEntityID;

            target->_control.stateChageTime = getFloatSteadyNowTime();
            notice.info.push_back(SceneEventInfo(master->_entityInfo.eid, target->_entityInfo.eid, SCENE_EVENT_LIE, 0, ""));
        }
    }
    broadcast(notice);
    return true;
}

void Scene::checkSceneState()
{
    SceneEventNotice eventNotice;
    for (auto kv : _entitys)
    {
        if (kv.second->_entityInfo.state == ENTITY_STATE_LIE || kv.second->_entityInfo.state == ENTITY_STATE_DIED)
        {
            if (kv.second->_entityInfo.etype == ENTITY_FLIGHT)
            {
                _asyncs.push(std::bind(&Scene::removeEntity, shared_from_this(), kv.second->_entityInfo.eid));
            }
            else if (kv.second->_control.stateChageTime + 10.0 < getFloatSteadyNowTime())
            {
                kv.second->_entityInfo.state = ENTITY_STATE_ACTIVE;
                kv.second->_entityInfo.curHP = kv.second->_props.hp;
                kv.second->_isInfoDirty = true;
                kv.second->_entityMove.position = kv.second->_control.spawnpoint;
                if ( kv.second->_control.agentNo < _sim->getNumAgents())
                {
                    _sim->setAgentPosition(kv.second->_control.agentNo, toRVOVector2(kv.second->_entityMove.position));
                }
                SceneEventInfo ev;
                ev.src = InvalidEntityID;
                ev.dst = kv.second->_entityInfo.eid;
                ev.ev = SCENE_EVENT_REBIRTH;
                ev.val = kv.second->_entityInfo.curHP;
                mergeToString(ev.mix, ",", kv.second->_entityMove.position.x);
                mergeToString(ev.mix, ",", kv.second->_entityMove.position.y);
                eventNotice.info.push_back(ev);
            }
        }
        broadcast(eventNotice);
    }
}
bool Scene::cleanSkill()
{
    return true;
}
bool Scene::addBuff()
{
    return true;
}
bool Scene::cleanBuff()
{
    return true;
}

std::vector<EntityPtr> Scene::searchTarget(EntityPtr caster, double radian, const SearchInfo & search)
{
    EntityPtr master = caster;
    std::vector<EntityPtr> ret;
    if (caster->_entityInfo.etype == ENTITY_FLIGHT  && caster->_entityInfo.leader != InvalidEntityID)
    {
        auto m = getEntity(caster->_entityInfo.leader);
        if (!m)
        {
            LOGE("");
            return ret;
        }
        master = m;
    }
    //偏移修正
    auto org = caster->_entityMove.position;
    if (true)
    {
        auto y = getFarPoint(caster->_entityMove.position.x, caster->_entityMove.position.y, radian, search.offsetY);
        auto x = getFarPoint(std::get<0>(y), std::get<1>(y), fmod(radian+ PI*2.0 - PI/2.0, PI*2.0), search.offsetX);
        org.x = std::get<0>(x);
        org.y = std::get<1>(x);
    }

    for (auto kv : _entitys)
    {
        auto & entity = *(kv.second);
        if (entity._entityInfo.etype != ENTITY_PLAYER && entity._entityInfo.etype != ENTITY_AI)
        {
            continue;
        }
        if (entity._entityInfo.state != ENTITY_STATE_ACTIVE)
        {
            continue;
        }
        if (getBitFlag(search.camp, SEARCH_CAMP_SELF) && entity._entityInfo.eid == master->_entityInfo.eid)
        {
            ret.push_back(kv.second);
            continue;
        }
        if (search.camp == SEARCH_CAMP_NONE
            ||(getBitFlag(search.camp, SEARCH_CAMP_SELF) && master->_entityInfo.eid == entity._entityInfo.eid)
            ||(getBitFlag(search.camp, SEARCH_CAMP_SAME_WITHOUT_SELF)
              && master->_entityInfo.camp == entity._entityInfo.camp
              && master->_entityInfo.eid != entity._entityInfo.eid)
            || (getBitFlag(search.camp, SEARCH_CAMP_ALIEN) && master->_entityInfo.camp != entity._entityInfo.camp))
        {
            //matched target camp
        }
        else
        {
            continue;
        }
        if (search.etype != ENTITY_NONE)
        {
            if (entity._entityInfo.etype != search.etype)
            {
                continue;
            }
        }

        if (getDistance(org, entity._entityMove.position) > search.distance)
        {
            continue;
        }
        if (search.method == SEARCH_METHOD_SEACTOR && search.radian < PI*2.0*0.9)
        {
            double radianEntity = getRadian(org.x, org.y, entity._entityMove.position.x, entity._entityMove.position.y);
            double curRadian = fmod(radian+search.radian/2.0, PI*2.0);
            if ((curRadian >= radianEntity && curRadian - radianEntity < search.radian)
                || (curRadian < radianEntity && curRadian + PI*2.0 - radianEntity < search.radian))
            {

            }
            else
            {
                continue;
            }
        }
        if (search.method == SEARCH_METHOD_RECT)
        {
            double curRadian = getRadian(org.x, org.y, entity._entityMove.position.x, entity._entityMove.position.y);
            auto dst = rotatePoint(org.x, org.y, curRadian,
                getDistance(org, entity._entityMove.position), PI*2.0 - curRadian );
            if (abs(std::get<1>(dst) - caster->_entityMove.position.y) > search.radian/2.0)
            {
                continue;
            }
        }
        ret.push_back(kv.second);
    }
    std::sort(ret.begin(), ret.end(), [caster](const EntityPtr & entity1, const EntityPtr & entity2)
    {return getDistance(entity1->_entityMove.position, caster->_entityMove.position) < getDistance(entity2->_entityMove.position, caster->_entityMove.position); });
    while (ret.size() > search.limitEntitys)
    {
        ret.pop_back();
    }
    return std::move(ret);
}



