﻿// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#pragma once

#include "Containers/Array.h"
#include "SpatialView/ComponentData.h"
#include "SpatialView/ComponentUpdate.h"
#include "SpatialView/OpList/OpList.h"
#include "Templates/UniquePtr.h"

namespace SpatialGDK
{
// Data for a set of ops representing
struct EntityComponentOpListData : OpListData
{
	TArray<Worker_Op> Ops;
	TArray<ComponentData> DataStorage;
	TArray<ComponentUpdate> UpdateStorage;
	TUniquePtr<char[]> DisconnectReason;
};

class EntityComponentOpListBuilder
{
public:
	EntityComponentOpListBuilder();

	EntityComponentOpListBuilder& AddEntity(Worker_EntityId EntityId);
	EntityComponentOpListBuilder& RemoveEntity(Worker_EntityId EntityId);
	EntityComponentOpListBuilder& AddComponent(Worker_EntityId EntityId, ComponentData Data);
	EntityComponentOpListBuilder& UpdateComponent(Worker_EntityId EntityId, ComponentUpdate Update);
	EntityComponentOpListBuilder& RemoveComponent(Worker_EntityId EntityId, Worker_ComponentId ComponentId);
	EntityComponentOpListBuilder& SetAuthority(Worker_EntityId EntityId, Worker_ComponentId ComponentId, Worker_Authority Authority);
	EntityComponentOpListBuilder& SetDisconnect(Worker_ConnectionStatusCode StatusCode, const FString& DisconnectReason);

	OpList CreateOpList() &&;

private:
	TUniquePtr<EntityComponentOpListData> OpListData;
};

} // namespace SpatialGDK