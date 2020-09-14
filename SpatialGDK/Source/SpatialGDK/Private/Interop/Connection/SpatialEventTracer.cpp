// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "Interop/Connection/SpatialEventTracer.h"

#include "Misc/ScopeLock.h"
#include "SpatialGDKSettings.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#include <WorkerSDK/improbable/c_io.h>
#include <WorkerSDK/improbable/c_trace.h>

DEFINE_LOG_CATEGORY(LogSpatialEventTracer);

#define DEBUG_EVENT_TRACING

using namespace SpatialGDK;
using namespace worker::c;

SpatialEventTracerGuard EventTracerGuard;

void SpatialEventTracer::TraceCallback(void* UserData, const Trace_Item* Item)
{
	FScopeLock ScopeLock(&EventTracerGuard.CriticalSection);

	SpatialEventTracer* EventTracer = EventTracerGuard.EventTracer;
	if (EventTracer == nullptr || !EventTracer->IsEnabled())
	{
		return;
	}

	if (!ensure(EventTracer->Stream.Get() != nullptr))
	{
		return;
	}

#ifdef DEBUG_EVENT_TRACING
	if (Item->item_type == TRACE_ITEM_TYPE_EVENT)
	{
		const Trace_Event& Event = Item->item.event;
		FString SpanIdString = SpatialEventTracer::SpanIdToString(Event.span_id);
		UE_LOG(LogSpatialEventTracer, Log, TEXT("Event: %s SpanId: %s"), *FString(Event.type), *SpanIdString);
	}
	else if (Item->item_type == TRACE_ITEM_TYPE_SPAN)
	{
		const Trace_Span& Span = Item->item.span;
		FString SpanIdString = SpanIdToString(Span.id);
		FString Causes;

		for (uint32 i = 0; i < Span.cause_count; ++i)
		{
			if (i > 0)
			{
				Causes += ", ";
			}

			Causes += SpanIdToString(Span.causes[i]);
		}

		UE_LOG(LogSpatialEventTracer, Log, TEXT("SpanId: %s Causes: %s"), *SpanIdString, *Causes);
	}
#endif // DEBUG_EVENT_TRACING

	uint32_t ItemSize = Trace_GetSerializedItemSize(Item);
	if (EventTracer->BytesWrittenToStream + ItemSize <= EventTracer->MaxFileSize)
	{
		EventTracer->BytesWrittenToStream += ItemSize;
		int Code = Trace_SerializeItemToStream(EventTracer->Stream.Get(), Item, ItemSize);
		if (Code != 1)
		{
			UE_LOG(LogSpatialEventTracer, Error, TEXT("Failed to serialize to with error code %d (%s"), Code, Trace_GetLastError());
		}
	}
}

SpatialScopedActiveSpanId::SpatialScopedActiveSpanId(SpatialEventTracer* InEventTracer, const TOptional<Trace_SpanId>& InCurrentSpanId)
	: CurrentSpanId(InCurrentSpanId)
	, EventTracer(InEventTracer->GetWorkerEventTracer())
{
	if (InCurrentSpanId.IsSet())
	{
		Trace_EventTracer_SetActiveSpanId(EventTracer, CurrentSpanId.GetValue());
	}
}

SpatialScopedActiveSpanId::~SpatialScopedActiveSpanId()
{
	if (CurrentSpanId.IsSet())
	{
		Trace_EventTracer_ClearActiveSpanId(EventTracer);
	}
}

SpatialEventTracer::SpatialEventTracer(const FString& WorkerId)
	: WorkerId(WorkerId)
{
	if (const USpatialGDKSettings* Settings = GetDefault<USpatialGDKSettings>())
	{
		if (Settings->bEventTracingEnabled)
		{
			MaxFileSize = Settings->MaxEventTracingFileSizeBytes;
			Enable(WorkerId);
		}
	}
}

SpatialEventTracer::~SpatialEventTracer()
{
	if (IsEnabled())
	{
		Disable();
	}
}

FString SpatialEventTracer::SpanIdToString(const Trace_SpanId& SpanId)
{
	FString HexStr;
	for (int i = 0; i < 16; i++)
	{
		{
			char b[32];
			unsigned int x = (unsigned char)SpanId.data[i];
			sprintf(b, "%0x", x);
			HexStr += ANSI_TO_TCHAR(b);
		}
	}
	return HexStr;
}

TOptional<Trace_SpanId> SpatialEventTracer::TraceEvent(const FEventMessage& EventMessage, const UStruct* Struct,
													   const TArray<worker::c::Trace_SpanId>& Causes)
{
	if (!IsEnabled())
	{
		return {};
	}

	Trace_SpanId CurrentSpanId;
	if (Causes.Num() > 0)
	{
		CurrentSpanId = Trace_EventTracer_AddSpan(EventTracer, Causes.GetData(), Causes.Num());
	}
	else
	{
		CurrentSpanId = Trace_EventTracer_AddSpan(EventTracer, nullptr, 0);
	}

	Trace_Event TraceEvent{ CurrentSpanId, /* unix_timestamp_millis: ignored */ 0, EventMessage.GetMessage(), EventMessage.GetType(),
							nullptr };
	if (!Trace_EventTracer_ShouldSampleEvent(EventTracer, &TraceEvent))
	{
		return {};
	}

	Trace_EventData* EventData = Trace_EventData_Create();

	auto AddTraceEventStringField = [EventData](const FString& KeyString, const FString& ValueString) {
		auto KeySrc = StringCast<ANSICHAR>(*KeyString);
		const ANSICHAR* Key = KeySrc.Get();
		auto ValueSrc = StringCast<ANSICHAR>(*ValueString);
		const ANSICHAR* Value = ValueSrc.Get();
		Trace_EventData_AddStringFields(EventData, 1, &Key, &Value);
	};

#if ENGINE_MINOR_VERSION >= 25
	using UnrealProperty = FProperty;
#else
	using UnrealProperty = UProperty;
#endif

	for (TFieldIterator<UnrealProperty> It(Struct); It; ++It)
	{
		UnrealProperty* Property = *It;

		FString VariableName = Property->GetName();
		const void* Value = Property->ContainerPtrToValuePtr<uint8>(&EventMessage);

		check(Property->ArrayDim == 1); // Arrays not handled yet

#if ENGINE_MINOR_VERSION >= 25
		if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
#else
		if (UStrProperty* StringProperty = Cast<UStrProperty>(Property))
#endif
		{
			AddTraceEventStringField(VariableName, StringProperty->GetPropertyValue(Value));
		}
#if ENGINE_MINOR_VERSION >= 25
		else if (FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
#else
		else if (UObjectProperty* ObjectProperty = Cast<UObjectProperty>(Property))
#endif
		{
			UObject* Object = ObjectProperty->GetPropertyValue(Value);
			if (Object)
			{
				AddTraceEventStringField(VariableName, Object->GetName());
				if (AActor* Actor = Cast<AActor>(Object))
				{
					FString KeyString = VariableName + TEXT("Position");
					FString ValueString = Actor->GetTransform().GetTranslation().ToString();
					AddTraceEventStringField(KeyString, ValueString);
				}
			}
			else
			{
				AddTraceEventStringField(VariableName, "Null");
			}
		}
		else // Default
		{
			FString StringValue;
			Property->ExportTextItem(StringValue, Value, NULL, NULL, PPF_None);
			AddTraceEventStringField(VariableName, StringValue);
		}

		// else if (UEnumProperty* EnumProperty = Cast<UEnumProperty>(Property))
		// else if (UNumericProperty *NumericProperty = Cast<UNumericProperty>(Property))
		// else if (UBoolProperty *BoolProperty = Cast<UBoolProperty>(Property))
		// else if (UTextProperty *TextProperty = Cast<UTextProperty>(Property))
		// else if (UArrayProperty *ArrayProperty = Cast<UArrayProperty>(Property))
		// else if (USetProperty* SetProperty = Cast<USetProperty>(Property))
		// else if (UMapProperty* MapProperty = Cast<UMapProperty>(Property))
		// else if (UStructProperty *StructProperty = Cast<UStructProperty>(Property))
	}

	// TODO(EventTracer): implement and call AddTargetObjectInfoToEventData
	// TODO(EventTracer): implement and call AddComponentIdInfoToEventData

	TraceEvent.data = EventData;
	Trace_EventTracer_AddEvent(EventTracer, &TraceEvent);
	Trace_EventData_Destroy(EventData);

	return CurrentSpanId;
}

bool SpatialEventTracer::IsEnabled() const
{
	return bEnabled; // Trace_EventTracer_IsEnabled(EventTracer);
}

void SpatialEventTracer::Enable(const FString& FileName)
{
	Trace_EventTracer_Parameters parameters = {};
	EventTracerGuard.EventTracer = this;
	parameters.user_data = &EventTracerGuard;
	parameters.callback = &SpatialEventTracer::TraceCallback;
	EventTracer = Trace_EventTracer_Create(&parameters);
	Trace_EventTracer_Enable(EventTracer);
	bEnabled = true;

	UE_LOG(LogSpatialEventTracer, Log, TEXT("Spatial event tracing enabled."));

	// Open a local file
	FolderPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("EventTracing"));
	const FString FullFileName = FString::Printf(TEXT("EventTrace_%s_%s.trace"), *FileName, *FDateTime::Now().ToString());
	const FString FilePath = FPaths::Combine(FolderPath, FullFileName);

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.CreateDirectoryTree(*FolderPath))
	{
		UE_LOG(LogSpatialEventTracer, Log, TEXT("Capturing trace to %s."), *FilePath);
		Stream.Reset(Io_CreateFileStream(TCHAR_TO_ANSI(*FilePath), Io_OpenMode::IO_OPEN_MODE_WRITE));
	}
}

void SpatialEventTracer::StreamDeleter::operator()(worker::c::Io_Stream* StreamToDestroy) const
{
	Io_Stream_Destroy(StreamToDestroy);
}

void SpatialEventTracer::Disable()
{
	UE_LOG(LogSpatialEventTracer, Log, TEXT("Spatial event tracing disabled."));

	FScopeLock ScopeLock(&EventTracerGuard.CriticalSection);
	EventTracerGuard.EventTracer = nullptr;

	Trace_EventTracer_Disable(EventTracer);
	bEnabled = false;
	Stream = nullptr;
	Trace_EventTracer_Destroy(EventTracer);
}

void SpatialEventTracer::ComponentAdd(const Worker_Op& Op)
{
	SpanIdStore.ComponentAdd(Op);
}

void SpatialEventTracer::ComponentRemove(const Worker_Op& Op)
{
	SpanIdStore.ComponentRemove(Op);
}

void SpatialEventTracer::ComponentUpdate(const Worker_Op& Op)
{
	const Worker_ComponentUpdateOp& ComponentUpdateOp = Op.op.component_update;
	EntityComponentId Id(ComponentUpdateOp.entity_id, ComponentUpdateOp.update.component_id);

	TArray<SpatialSpanIdStore::FieldSpanIdUpdate> FieldSpanIdUpdates = SpanIdStore.ComponentUpdate(Op);
	for (const SpatialSpanIdStore::FieldSpanIdUpdate& FieldSpanIdUpdate : FieldSpanIdUpdates)
	{
		uint32 FieldId = FieldSpanIdUpdate.FieldId;
		TArray<Trace_SpanId> MergeCauses = { FieldSpanIdUpdate.NewSpanId, FieldSpanIdUpdate.OldSpanId };

		TOptional<Trace_SpanId> NewSpanId = TraceEvent(FEventMergeComponentFieldUpdate(Id.EntityId, Id.ComponentId, FieldId), MergeCauses);
		SpanIdStore.WriteSpanId(Id, FieldId, NewSpanId.GetValue());
	}
}

worker::c::Trace_SpanId SpatialEventTracer::GetSpanId(const EntityComponentId& Id, const uint32 FieldId)
{
	return SpanIdStore.GetSpanId(Id, FieldId);
}

void SpatialEventTracer::DropSpanIds(const EntityComponentId& Id)
{
	SpanIdStore.DropSpanIds(Id);
}

void SpatialEventTracer::DropSpanId(const EntityComponentId& Id, const uint32 FieldId)
{
	SpanIdStore.DropSpanId(Id, FieldId);
}

void SpatialEventTracer::DropOldSpanIds()
{
	SpanIdStore.DropOldSpanIds();
}
