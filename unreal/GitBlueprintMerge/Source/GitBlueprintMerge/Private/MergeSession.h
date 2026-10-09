// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#pragma once

#include "AssetMerge.h"

class UBlueprint;
class UPackage;

namespace GitBlueprintMerge
{
	/** One revision of the asset, copied under a unique name into Saved/Diff and loaded from there. */
	struct FRevision
	{
		FString TempFile;
		UPackage* Package = nullptr;
		UObject* Asset = nullptr;
		/** Set when Asset is a Blueprint. */
		UBlueprint* Blueprint = nullptr;
	};

	/**
	 * Base / Ours / Theirs of one asset (Blueprint, data asset, data table, material instance) loaded side by side, the merge plan between them, and the
	 * merge itself. Shared by the commandlet and the in-editor merge window.
	 */
	class FMergeSession
	{
	public:
		~FMergeSession();

		bool Load(const FString& InBaseFile, const FString& InOursFile, const FString& InTheirsFile);

		const FMergePlan& GetPlan() const { return Plan; }
		UBlueprint* GetBase() const { return Base.Blueprint; }
		UBlueprint* GetOurs() const { return Ours.Blueprint; }
		UBlueprint* GetTheirs() const { return Theirs.Blueprint; }
		UObject* GetBaseAsset() const { return Base.Asset; }
		UObject* GetOursAsset() const { return Ours.Asset; }
		UObject* GetTheirsAsset() const { return Theirs.Asset; }
		EAssetKind GetKind() const { return Kind; }
		FString GetAssetName() const;

		FString Describe(const FDecision& Decision) const;
		FConflictSummary Summarize(int32 Group) const;
		FString Subject(const FString& Key) const { return DescribeSubject(Key, BaseSnap, OursSnap, TheirsSnap); }
		FString ValueText(const FString& Key, const TOptional<FString>& Value) const { return DescribeValue(Key, Value, BaseSnap, OursSnap, TheirsSnap); }

		/** True when both sides' nodes of the conflict can be kept, one after the other (see TakeBoth). */
		bool CanTakeBoth(int32 Group) const;

		/**
		 * Builds the merged result for these choices in memory (undecided conflicts as ours, unsupported changes
		 * left out), compiled, so it can be shown before merging. Replaces the previous preview.
		 */
		void UpdatePreview(const TMap<int32, EResolution>& Choices);
		UBlueprint* GetPreview() const { return Preview.Blueprint; }
		UObject* GetPreviewAsset() const { return Preview.Asset; }
		const FSnapshot& GetPreviewSnapshot() const { return PreviewSnap; }
		/** Why the preview doesn't compile or couldn't be built; empty when it's fine. */
		const TArray<FString>& GetPreviewProblems() const { return PreviewProblems; }

		/** Changes from theirs the merge can't take yet, in words (conflicts say "if you take theirs"). */
		const TArray<FString>& GetUnsupported() const { return Unsupported; }

		/** True when, with these choices (undecided = ours), an unsupported change would be taken from theirs. */
		bool NeedsLeaveOut(const TMap<int32, EResolution>& Choices) const;

		/**
		 * Resolves the conflicts with Choices (group -> Ours/Theirs), applies the plan onto a fresh copy of
		 * Ours, compiles it and saves it to OutputFile. Can be retried; the loaded revisions stay untouched.
		 * Unsupported changes from theirs stop the merge, unless bLeaveOutUnsupported keeps ours for them.
		 */
		bool Merge(const TMap<int32, EResolution>& Choices, const FString& OutputFile, const FString& PackageName, bool bLeaveOutUnsupported = false);

		/**
		 * The person edited the preview by hand: compiles it (Blueprints) and takes its snapshot again, so the
		 * preview problems and values match what is on screen. False when it doesn't compile.
		 */
		bool CheckEditedPreview();

		/** Snapshot of the preview again, without compiling (cheap enough to do after every edit). */
		void RefreshPreviewSnapshot();

		/**
		 * Like Merge, but saves the preview as the person edited it by hand instead of building the result again.
		 * Choices and bLeaveOutUnsupported must match the preview; they also fill the report.
		 */
		bool MergeEdited(const TMap<int32, EResolution>& Choices, const FString& OutputFile, const FString& PackageName, bool bLeaveOutUnsupported = false);

		bool WasWritten() const { return bWritten; }
		const TArray<FString>& GetProblems() const { return Problems; }

		/** What the last merge left out (see bLeaveOutUnsupported), in words. */
		const TArray<FString>& GetLeftOut() const { return LeftOut; }

		void LogPlan() const;
		bool WriteReport(const FString& File) const;

	private:
		FString OursFile;
		EAssetKind Kind = EAssetKind::Unsupported;
		FRevision Base;
		FRevision Ours;
		FRevision Theirs;
		TArray<FRevision> Results;
		FSnapshot BaseSnap;
		FSnapshot OursSnap;
		FSnapshot TheirsSnap;
		FMergePlan Plan;
		FMergePlan MergedPlan;
		TArray<FString> Unsupported;
		TArray<FString> Problems;
		TArray<FString> LeftOut;
		bool bWritten = false;
		bool bEditedByHand = false;

		FRevision Preview;
		TArray<FRevision> OldPreviews;
		FSnapshot PreviewSnap;
		TArray<FString> PreviewProblems;

		/** Text for one unsupported change found in a plan. */
		FString DescribeUnsupported(const FDecision& Decision) const;

		/**
		 * Plan with the choices applied (take-both groups chained). Undecided conflicts count as ours when
		 * bUndecidedAsOurs, otherwise they make it fail. OutMoveAside: nodes to place below the others.
		 */
		bool Resolve(FMergePlan& OutPlan, const TMap<int32, EResolution>& Choices, bool bUndecidedAsOurs, TArray<FString>& OutMoveAside) const;

		/** Resets the last merge's outcome and fills MergedPlan / LeftOut for these choices; false (with Problems) when it can't merge. */
		bool PlanMergeFor(const TMap<int32, EResolution>& Choices, bool bLeaveOutUnsupported, TArray<FString>& OutMoveAside);

		/** Every revision that may hold the real package name from an earlier attempt. */
		TArray<FRevision> EarlierResults() const;

		/** Applies the plan's changes from theirs onto Result (a fresh copy of ours). */
		bool Apply(const FMergePlan& InPlan, FRevision& Result, const TArray<FString>& MoveAsideIds, TArray<FString>& OutProblems) const;

		/** Moves nodes a take-both put next to each other, so they don't sit on top of one another. */
		static void MoveAside(UBlueprint* Blueprint, const TArray<FString>& NodeIds);
	};

	/** "ours" / "theirs" -> resolution; anything else -> Unchanged. */
	EResolution ParseSide(const FString& Side);

	/** Copies SourceFile into Saved/Diff under a unique name and loads the asset in it (a kind AssetMerge.h supports). */
	bool LoadRevision(const FString& SourceFile, const TCHAR* Label, FRevision& Out);

	/** Unloads the revision's file handle and deletes its temp copy. */
	void ReleaseRevision(FRevision& Revision);

	/**
	 * Gives the revision's package its real name (PackageName, or the name OutputFile maps to) so the saved
	 * file doesn't carry the temp name. Do it before editing, so copied references use the real name too.
	 * PreviousResults may hold that name from an earlier attempt; they are moved out of the way.
	 */
	void RenameRevision(FRevision& Revision, const FString& PackageName, const FString& OutputFile, TConstArrayView<FRevision> PreviousResults = {});

	bool SaveRevision(const FRevision& Revision, const FString& OutputFile, TArray<FString>& OutProblems);
}
