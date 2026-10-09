// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#include "GitBlueprintMergeCommandlet.h"

#include "Dom/JsonObject.h"
#include "MergeSession.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GitBlueprintMergeCommandlet)

DEFINE_LOG_CATEGORY_STATIC(LogGitBlueprintMergeCommandlet, Log, All);

namespace
{
	using namespace GitBlueprintMerge;

	/** -Choices=<json file> ({"0": "ours", "1": "theirs"}) and/or -Prefer=ours|theirs for every conflict. */
	TMap<int32, EResolution> ReadChoices(const FString& Params, const FMergePlan& Plan)
	{
		TMap<int32, EResolution> Choices;

		FString Prefer;
		if (FParse::Value(*Params, TEXT("Prefer="), Prefer) && ParseSide(Prefer) != EResolution::Unchanged)
		{
			for (int32 Group = 0; Group < Plan.NumGroups; ++Group)
				Choices.Add(Group, ParseSide(Prefer));
		}

		FString ChoicesFile, Json;
		if (FParse::Value(*Params, TEXT("Choices="), ChoicesFile) && FFileHelper::LoadFileToString(Json, *ChoicesFile))
		{
			TSharedPtr<FJsonObject> Root;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) && Root.IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
					Choices.Add(FCString::Atoi(*Pair.Key), ParseSide(Pair.Value->AsString()));
			}
		}

		return Choices;
	}
}

UGitBlueprintMergeCommandlet::UGitBlueprintMergeCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false;
}

int32 UGitBlueprintMergeCommandlet::Main(const FString& Params)
{
	FString BaseFile, OursFile, TheirsFile, ReportFile, OutputFile, PackageName;
	FParse::Value(*Params, TEXT("Base="), BaseFile);
	FParse::Value(*Params, TEXT("Ours="), OursFile);
	FParse::Value(*Params, TEXT("Theirs="), TheirsFile);
	FParse::Value(*Params, TEXT("Report="), ReportFile);
	FParse::Value(*Params, TEXT("Output="), OutputFile);
	FParse::Value(*Params, TEXT("PackageName="), PackageName);

	if (BaseFile.IsEmpty() || OursFile.IsEmpty() || TheirsFile.IsEmpty())
	{
		UE_LOG(LogGitBlueprintMergeCommandlet, Error, TEXT("Usage: -run=GitBlueprintMerge -Base=<file> -Ours=<file> -Theirs=<file> [-Report=<json>] ")
			TEXT("[-Output=<file> [-PackageName=<name>] [-Choices=<json> | -Prefer=ours|theirs] [-LeaveOutUnsupported]]"));
		return 2;
	}

	FMergeSession Session;
	if (!Session.Load(BaseFile, OursFile, TheirsFile))
		return 2;

	Session.LogPlan();

	// Write the merged result when asked and every conflict has a decision.
	const TMap<int32, EResolution> Choices = ReadChoices(Params, Session.GetPlan());
	const bool bDecided = Session.GetPlan().NumConflicts == 0 || Choices.Num() >= Session.GetPlan().NumGroups;
	if (!OutputFile.IsEmpty() && bDecided)
		Session.Merge(Choices, OutputFile, PackageName, FParse::Param(*Params, TEXT("LeaveOutUnsupported")));

	if (!ReportFile.IsEmpty())
		Session.WriteReport(ReportFile);

	if (Session.WasWritten())
		return 0;
	return Session.GetPlan().NumConflicts > 0 && !bDecided ? 1 : (OutputFile.IsEmpty() ? 0 : 3);
}
