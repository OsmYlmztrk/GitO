// Copyright (c) MyGitGUI contributors. MIT License.

#include "MergeSession.h"

#include "BlueprintMergeApply.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogGitBlueprintMerge, Log, All);

namespace GitBlueprintMerge
{
	namespace
	{
		TSharedRef<FJsonValue> ToJson(const TOptional<FString>& Value)
		{
			return Value.IsSet() ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Value.GetValue())) : StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>());
		}
	}

	/**
	 * All revisions share one package name, so each is copied under a unique name into
	 * Saved/Diff (mounted as /Temp/) and loaded from there side by side.
	 */
	bool LoadRevision(const FString& SourceFile, const TCHAR* Label, FRevision& Out)
	{
		const FString Name = FString::Printf(TEXT("GBM_%s_%s"), Label, *FGuid::NewGuid().ToString());
		Out.TempFile = FPaths::Combine(FPaths::DiffDir(), Name) + FPackageName::GetAssetPackageExtension();

		if (IFileManager::Get().Copy(*Out.TempFile, *SourceFile) != COPY_OK)
		{
			UE_LOG(LogGitBlueprintMerge, Error, TEXT("Could not copy %s revision '%s'."), Label, *SourceFile);
			return false;
		}

		Out.Package = LoadPackage(nullptr, *Out.TempFile, LOAD_None);
		Out.Asset = Out.Package ? Out.Package->FindAssetInPackage() : nullptr;
		Out.Blueprint = Cast<UBlueprint>(Out.Asset);
		if (GetAssetKind(Out.Asset) == EAssetKind::Unsupported)
		{
			UE_LOG(LogGitBlueprintMerge, Error, TEXT("%s revision '%s' is a %s; only Blueprints, Data Assets, Data Tables and Material Instances can be merged."),
				Label, *SourceFile, Out.Asset ? *Out.Asset->GetClass()->GetName() : TEXT("package without an asset"));
			return false;
		}

		return true;
	}

	void ReleaseRevision(FRevision& Revision)
	{
		if (Revision.Package)
			ResetLoaders(Revision.Package);
		if (!Revision.TempFile.IsEmpty())
			IFileManager::Get().Delete(*Revision.TempFile, false, false, true);
		Revision = FRevision();
	}

	/**
	 * Gives the merged package its real name (e.g. /Game/Folder/BP_Thing) before saving, so the saved
	 * file doesn't carry the temp name in its class paths and asset registry tags.
	 */
	void RenameRevision(FRevision& Revision, const FString& InPackageName, const FString& OutputFile, TConstArrayView<FRevision> PreviousResults)
	{
		FString PackageName = InPackageName;
		if (PackageName.IsEmpty() && !FPackageName::TryConvertFilenameToLongPackageName(OutputFile, PackageName))
		{
			UE_LOG(LogGitBlueprintMerge, Warning, TEXT("'%s' is not inside a mounted content folder; pass -PackageName to name the result."), *OutputFile);
			return;
		}

		if (UPackage* Existing = FindPackage(nullptr, *PackageName))
		{
			// A previous merge attempt holds the name; move it out of the way.
			const bool bPrevious = PreviousResults.ContainsByPredicate([Existing](const FRevision& Previous) { return Previous.Package == Existing; });
			if (!bPrevious)
			{
				UE_LOG(LogGitBlueprintMerge, Warning, TEXT("Package %s is already loaded; keeping the temporary name."), *PackageName);
				return;
			}

			Existing->Rename(*FString::Printf(TEXT("/Temp/Diff/GBM_Discarded_%s"), *FGuid::NewGuid().ToString()), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
		}

		Revision.Package->Rename(*PackageName, nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
	}

	bool SaveRevision(const FRevision& Revision, const FString& OutputFile, TArray<FString>& OutProblems)
	{
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Standalone;
		Args.Error = GLog;

		const FSavePackageResultStruct Result = UPackage::Save(Revision.Package, Revision.Asset, *OutputFile, Args);
		if (Result.Result != ESavePackageResult::Success)
		{
			OutProblems.Add(FString::Printf(TEXT("Could not save '%s'."), *OutputFile));
			return false;
		}
		return true;
	}

	EResolution ParseSide(const FString& Side)
	{
		if (Side.Equals(TEXT("ours"), ESearchCase::IgnoreCase))
			return EResolution::Ours;
		if (Side.Equals(TEXT("theirs"), ESearchCase::IgnoreCase))
			return EResolution::Theirs;
		if (Side.Equals(TEXT("both"), ESearchCase::IgnoreCase))
			return EResolution::Both;
		if (Side.Equals(TEXT("both-theirs-first"), ESearchCase::IgnoreCase))
			return EResolution::BothTheirsFirst;
		return EResolution::Unchanged;
	}

	FMergeSession::~FMergeSession()
	{
		ReleaseRevision(Base);
		ReleaseRevision(Ours);
		ReleaseRevision(Theirs);
		ReleaseRevision(Preview);
		for (FRevision& Old : OldPreviews)
			ReleaseRevision(Old);
		for (FRevision& Result : Results)
			ReleaseRevision(Result);
	}

	bool FMergeSession::CanTakeBoth(int32 Group) const
	{
		FInsertion OursPart, TheirsPart;
		return FindInsertions(Plan, Group, OursSnap, TheirsSnap, OursPart, TheirsPart);
	}

	bool FMergeSession::Resolve(FMergePlan& OutPlan, const TMap<int32, EResolution>& Choices, bool bUndecidedAsOurs, TArray<FString>& OutMoveAside) const
	{
		OutPlan = Plan;
		TMap<int32, EResolution> Sides;
		for (int32 Group = 0; Group < Plan.NumGroups; ++Group)
		{
			const EResolution* Choice = Choices.Find(Group);
			if (Choice && (*Choice == EResolution::Both || *Choice == EResolution::BothTheirsFirst))
			{
				const TArray<FString> Second = TakeBoth(OutPlan, Group, *Choice == EResolution::Both, OursSnap, TheirsSnap);
				if (Second.IsEmpty())
					return false;
				OutMoveAside.Append(Second);
			}
			else if (Choice)
			{
				Sides.Add(Group, *Choice);
			}
			else if (bUndecidedAsOurs)
			{
				Sides.Add(Group, EResolution::Ours);
			}
		}
		return ResolveConflicts(OutPlan, Sides);
	}

	bool FMergeSession::Apply(const FMergePlan& InPlan, FRevision& Result, const TArray<FString>& MoveAsideIds, TArray<FString>& OutProblems) const
	{
		if (Kind != EAssetKind::Blueprint)
			return ApplyTheirsToData(InPlan, Result.Asset, Theirs.Asset, OutProblems);

		if (!ApplyTheirs(InPlan, Result.Blueprint, Theirs.Blueprint, OutProblems))
			return false;
		MoveAside(Result.Blueprint, MoveAsideIds);
		return true;
	}

	void FMergeSession::MoveAside(UBlueprint* Blueprint, const TArray<FString>& NodeIds)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && NodeIds.Contains(Node->NodeGuid.ToString()))
					Node->NodePosY += 220;
			}
		}
	}

	void FMergeSession::UpdatePreview(const TMap<int32, EResolution>& Choices)
	{
		TArray<FString> NewProblems;
		FRevision Next;

		FMergePlan Previewed;
		TArray<FString> MoveAsideIds;
		if (!Resolve(Previewed, Choices, /*bUndecidedAsOurs=*/true, MoveAsideIds))
		{
			NewProblems.Add(TEXT("These choices can't be combined."));
		}
		else if (!LoadRevision(OursFile, TEXT("Preview"), Next))
		{
			NewProblems.Add(TEXT("Could not load a working copy of ours."));
		}
		else
		{
			if (FindUnsupported(Previewed, OursSnap, TheirsSnap).Num() > 0)
				LeaveOutUnsupported(Previewed, OursSnap, TheirsSnap);
			Apply(Previewed, Next, MoveAsideIds, NewProblems);
		}

		// The old preview may still be on screen until the views are rebuilt; let it go with the session.
		if (Preview.Package)
			OldPreviews.Add(Preview);
		Preview = Next;
		PreviewSnap = Preview.Asset ? TakeAssetSnapshot(Preview.Asset) : FSnapshot();
		PreviewProblems = NewProblems;
	}

	bool FMergeSession::Load(const FString& InBaseFile, const FString& InOursFile, const FString& InTheirsFile)
	{
		OursFile = InOursFile;
		if (!LoadRevision(InBaseFile, TEXT("Base"), Base) ||
			!LoadRevision(InOursFile, TEXT("Ours"), Ours) ||
			!LoadRevision(InTheirsFile, TEXT("Theirs"), Theirs))
			return false;

		Kind = GetAssetKind(Ours.Asset);
		if (GetAssetKind(Base.Asset) != Kind || GetAssetKind(Theirs.Asset) != Kind)
		{
			UE_LOG(LogGitBlueprintMerge, Error, TEXT("The three revisions are different kinds of assets."));
			return false;
		}

		BaseSnap = TakeAssetSnapshot(Base.Asset);
		OursSnap = TakeAssetSnapshot(Ours.Asset);
		TheirsSnap = TakeAssetSnapshot(Theirs.Asset);
		Plan = PlanMerge(BaseSnap, OursSnap, TheirsSnap);

		for (int32 Index : FindUnsupported(Plan, OursSnap, TheirsSnap))
			Unsupported.Add(DescribeUnsupported(Plan.Decisions[Index]));
		return true;
	}

	FString FMergeSession::DescribeUnsupported(const FDecision& Decision) const
	{
		if (Decision.Resolution != EResolution::Conflict)
			return Describe(Decision);

		FDecision Taken = Decision;
		Taken.Resolution = EResolution::Theirs;
		return Describe(Taken) + TEXT(" (only if you take theirs in that conflict)");
	}

	bool FMergeSession::NeedsLeaveOut(const TMap<int32, EResolution>& Choices) const
	{
		if (Unsupported.IsEmpty())
			return false;

		FMergePlan Trial;
		TArray<FString> Unused;
		Resolve(Trial, Choices, /*bUndecidedAsOurs=*/true, Unused);
		return FindUnsupported(Trial, OursSnap, TheirsSnap).Num() > 0;
	}

	FString FMergeSession::GetAssetName() const
	{
		return Ours.Asset ? Ours.Asset->GetName() : FString();
	}

	FString FMergeSession::Describe(const FDecision& Decision) const
	{
		return GitBlueprintMerge::Describe(Decision, BaseSnap, OursSnap, TheirsSnap);
	}

	FConflictSummary FMergeSession::Summarize(int32 Group) const
	{
		return SummarizeGroup(Plan, Group, BaseSnap, OursSnap, TheirsSnap);
	}

	TArray<FRevision> FMergeSession::EarlierResults() const
	{
		TArray<FRevision> All = Results;
		All.Append(OldPreviews);
		if (Preview.Package)
			All.Add(Preview);
		return All;
	}

	bool FMergeSession::PlanMergeFor(const TMap<int32, EResolution>& Choices, bool bLeaveOutUnsupported, TArray<FString>& OutMoveAside)
	{
		Problems.Reset();
		LeftOut.Reset();
		bWritten = false;
		bEditedByHand = false;

		if (!Resolve(MergedPlan, Choices, /*bUndecidedAsOurs=*/false, OutMoveAside))
		{
			Problems.Add(TEXT("Every conflict needs a choice (ours, theirs or both)."));
			return false;
		}

		// Say what can't be merged up front, in words, instead of failing halfway.
		const TArray<int32> Blocking = FindUnsupported(MergedPlan, OursSnap, TheirsSnap);
		if (Blocking.Num() > 0)
		{
			TArray<FString> Texts;
			for (int32 Index : Blocking)
				Texts.Add(Describe(MergedPlan.Decisions[Index]));

			if (!bLeaveOutUnsupported)
			{
				Problems.Add(FString::Printf(TEXT("These changes from theirs can't be merged automatically yet: %s. Leave them out and redo them by hand, or keep one side of the whole asset."),
					*FString::Join(Texts, TEXT("; "))));
				UE_LOG(LogGitBlueprintMerge, Warning, TEXT("Not merged: %s"), *Problems.Last());
				return false;
			}

			LeftOut = Texts;
			LeaveOutUnsupported(MergedPlan, OursSnap, TheirsSnap);
			for (const FString& Text : LeftOut)
				UE_LOG(LogGitBlueprintMerge, Display, TEXT("Left out (redo by hand): %s"), *Text);
		}
		return true;
	}

	bool FMergeSession::Merge(const TMap<int32, EResolution>& Choices, const FString& OutputFile, const FString& PackageName, bool bLeaveOutUnsupported)
	{
		TArray<FString> MoveAsideIds;
		if (!PlanMergeFor(Choices, bLeaveOutUnsupported, MoveAsideIds))
			return false;

		// Work on a fresh copy of ours so a failed attempt can be retried with other choices.
		FRevision Result;
		if (!LoadRevision(OursFile, TEXT("Result"), Result))
		{
			Problems.Add(TEXT("Could not load a working copy of ours."));
			ReleaseRevision(Result);
			return false;
		}

		RenameRevision(Result, PackageName, OutputFile, EarlierResults());
		Results.Add(Result);
		if (Apply(MergedPlan, Result, MoveAsideIds, Problems))
			bWritten = SaveRevision(Result, OutputFile, Problems);

		for (const FString& Problem : Problems)
			UE_LOG(LogGitBlueprintMerge, Warning, TEXT("Not merged: %s"), *Problem);
		if (bWritten)
			UE_LOG(LogGitBlueprintMerge, Display, TEXT("Merged result saved to '%s'."), *OutputFile);

		return bWritten;
	}

	bool FMergeSession::CheckEditedPreview()
	{
		PreviewProblems.Reset();
		if (!Preview.Asset)
			PreviewProblems.Add(TEXT("There is no result to check."));
		else if (Preview.Blueprint)
			CompileMerged(Preview.Blueprint, PreviewProblems);

		RefreshPreviewSnapshot();
		return PreviewProblems.IsEmpty();
	}

	void FMergeSession::RefreshPreviewSnapshot()
	{
		PreviewSnap = Preview.Asset ? TakeAssetSnapshot(Preview.Asset) : FSnapshot();
	}

	bool FMergeSession::MergeEdited(const TMap<int32, EResolution>& Choices, const FString& OutputFile, const FString& PackageName, bool bLeaveOutUnsupported)
	{
		TArray<FString> Unused;
		if (!PlanMergeFor(Choices, bLeaveOutUnsupported, Unused))
			return false;
		if (!Preview.Package || !Preview.Asset)
		{
			Problems.Add(TEXT("There is no edited result to save."));
			return false;
		}

		bEditedByHand = true;
		RenameRevision(Preview, PackageName, OutputFile, EarlierResults());
		if (!Preview.Blueprint || CompileMerged(Preview.Blueprint, Problems))
			bWritten = SaveRevision(Preview, OutputFile, Problems);

		for (const FString& Problem : Problems)
			UE_LOG(LogGitBlueprintMerge, Warning, TEXT("Not merged: %s"), *Problem);
		if (bWritten)
			UE_LOG(LogGitBlueprintMerge, Display, TEXT("Merged result, edited by hand, saved to '%s'."), *OutputFile);

		return bWritten;
	}

	void FMergeSession::LogPlan() const
	{
		UE_LOG(LogGitBlueprintMerge, Display, TEXT("%s '%s': %d fact(s) compared."), AssetKindName(Kind), *GetAssetName(), Plan.Decisions.Num());
		for (const FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Resolution == EResolution::Unchanged || Decision.bPartOfWhole)
				continue;

			const FString Group = Decision.Group == INDEX_NONE ? FString() : FString::Printf(TEXT(" #%d"), Decision.Group);
			UE_LOG(LogGitBlueprintMerge, Display, TEXT("  [%-8s%s] %s"), ToString(Decision.Resolution), *Group, *Describe(Decision));
		}

		for (int32 Group = 0; Group < Plan.NumGroups; ++Group)
		{
			const FConflictSummary Summary = Summarize(Group);
			UE_LOG(LogGitBlueprintMerge, Display, TEXT("Conflict #%d: %s | ours: %s | theirs: %s"), Group, *Summary.Title, *Summary.Ours, *Summary.Theirs);
		}

		for (const FString& Text : Unsupported)
			UE_LOG(LogGitBlueprintMerge, Display, TEXT("Not supported yet: %s"), *Text);

		UE_LOG(LogGitBlueprintMerge, Display, TEXT("Summary: %d from ours, %d from theirs, %d identical on both, %d conflict(s) to decide."),
			Plan.NumOurs, Plan.NumTheirs, Plan.NumBoth, Plan.NumConflicts);
	}

	bool FMergeSession::WriteReport(const FString& File) const
	{
		// After a merge attempt, report what was (or would have been) merged with the choices made.
		const bool bAttempted = MergedPlan.Decisions.Num() > 0;
		const FMergePlan& Shown = bAttempted ? MergedPlan : Plan;

		TArray<TSharedPtr<FJsonValue>> Changes;
		for (const FDecision& Decision : Shown.Decisions)
		{
			if (Decision.Resolution == EResolution::Unchanged || Decision.bPartOfWhole)
				continue;

			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("key"), Decision.Key);
			Change->SetStringField(TEXT("resolution"), ToString(Decision.Resolution));
			Change->SetStringField(TEXT("text"), Describe(Decision));
			Change->SetField(TEXT("base"), ToJson(Decision.Base));
			Change->SetField(TEXT("ours"), ToJson(Decision.Ours));
			Change->SetField(TEXT("theirs"), ToJson(Decision.Theirs));
			Change->SetNumberField(TEXT("group"), Decision.Group);
			if (!Decision.Reason.IsEmpty())
				Change->SetStringField(TEXT("reason"), Decision.Reason);
			Changes.Add(MakeShared<FJsonValueObject>(Change));
		}

		TArray<TSharedPtr<FJsonValue>> Conflicts;
		for (int32 Group = 0; Group < Plan.NumGroups; ++Group)
		{
			TArray<TSharedPtr<FJsonValue>> Details;
			for (const FDecision& Decision : Plan.Decisions)
			{
				if (Decision.Group == Group && IsWorthShowing(Decision))
					Details.Add(MakeShared<FJsonValueString>(Describe(Decision)));
			}

			const FConflictSummary Summary = Summarize(Group);
			TSharedRef<FJsonObject> Conflict = MakeShared<FJsonObject>();
			Conflict->SetNumberField(TEXT("group"), Group);
			Conflict->SetStringField(TEXT("text"), Summary.Title);
			Conflict->SetStringField(TEXT("ours"), Summary.Ours);
			Conflict->SetStringField(TEXT("theirs"), Summary.Theirs);
			Conflict->SetArrayField(TEXT("details"), Details);
			Conflicts.Add(MakeShared<FJsonValueObject>(Conflict));
		}

		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("ours"), Shown.NumOurs);
		Summary->SetNumberField(TEXT("theirs"), Shown.NumTheirs);
		Summary->SetNumberField(TEXT("both"), Shown.NumBoth);
		Summary->SetNumberField(TEXT("conflicts"), Shown.NumConflicts);
		Summary->SetNumberField(TEXT("resolved"), bAttempted ? Plan.NumConflicts - Shown.NumConflicts : 0);

		auto ToJsonArray = [](const TArray<FString>& Texts)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Text : Texts)
				Values.Add(MakeShared<FJsonValueString>(Text));
			return Values;
		};

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("asset"), GetAssetName());
		Root->SetStringField(TEXT("kind"), AssetKindName(Kind));
		Root->SetBoolField(TEXT("canAutoMerge"), Plan.NumConflicts == 0 && Unsupported.IsEmpty());
		Root->SetBoolField(TEXT("written"), bWritten);
		Root->SetBoolField(TEXT("editedByHand"), bWritten && bEditedByHand);
		Root->SetObjectField(TEXT("summary"), Summary);
		Root->SetArrayField(TEXT("conflicts"), Conflicts);
		Root->SetArrayField(TEXT("changes"), Changes);
		Root->SetArrayField(TEXT("unsupported"), ToJsonArray(Unsupported));
		Root->SetArrayField(TEXT("leftOut"), ToJsonArray(LeftOut));
		Root->SetArrayField(TEXT("problems"), ToJsonArray(Problems));

		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Root, Writer);
		if (!FFileHelper::SaveStringToFile(Json, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogGitBlueprintMerge, Error, TEXT("Could not write report '%s'."), *File);
			return false;
		}
		return true;
	}
}
