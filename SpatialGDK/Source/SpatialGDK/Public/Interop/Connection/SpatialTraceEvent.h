// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#pragma once

namespace SpatialGDK
{
struct FSpatialTraceEvent
{
	FSpatialTraceEvent(const char* InType, FString InMessage)
		: Type(InType)
		, Message(MoveTemp(InMessage))
	{
	}

	void AddData(FString Key, FString Value) { Data.Add(MakeTuple(MoveTemp(Key), MoveTemp(Value))); }

	const char* Type;
	FString Message;
	TArray<TTuple<FString, FString>> Data;
};
} // namespace SpatialGDK
