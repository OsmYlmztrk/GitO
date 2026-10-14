// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterActorList.h"

class AActor;
class UPackage;
class UWorld;

namespace GitBlueprintMerge
{
	/** Nothing is decided for the user: every change starts as None. Level keeps the value the level has now. */
	enum class ELevelChoice : uint8
	{
		None,
		Ours,
		Theirs,
		Level,
	};

	/** Who changed a property since base. */
	enum class ELevelChangeKind : uint8
	{
		Both,       // both sides, to different values
		Same,       // both sides, to the same value
		OursOnly,
		TheirsOnly,
	};

	/** One property that differs between the three revisions of an actor. */
	struct FLevelChange
	{
		FString Key;        // actor/<prop> or comp/<component>/<prop>
		FString Name;       // readable property name: "Light Color"
		FString Component;  // owning component, empty for the actor itself
		FString Category;   // the property's top Details category: "Light", "Transform"
		FString Base;
		FString Ours;
		FString Theirs;
		bool bInBase = true;
		bool bInOurs = true;
		bool bInTheirs = true;
		ELevelChangeKind Kind = ELevelChangeKind::Both;
		ELevelChoice Choice = ELevelChoice::None;

		/** The level's value right after a side was chosen; a different value later means it was edited by hand. */
		FString Applied;
	};

	/** A conflicted actor file of the level. */
	struct FLevelActor
	{
		FString Path;          // repo-relative file, as GitO knows it
		FString PackageName;   // /Game/__ExternalActors__/<Level>/X/YY/<hash>
		FString Label;
		FString ClassName;
		bool bInBase = false;
		bool bInOurs = false;
		bool bInTheirs = false;

		/** The actor in the editor's level: ours, or theirs when ours deleted it. */
		TWeakObjectPtr<AActor> Live;

		/** Set when one side deleted the actor: Ours / Theirs = keep that side's decision. */
		bool bExistenceConflict = false;
		ELevelChoice Existence = ELevelChoice::None;

		/** Both sides kept the actor: the properties to decide. */
		TArray<FLevelChange> Changes;

		/** One side deleted the actor: what the other side changed since base (shown, not decided). */
		TArray<FLevelChange> KeptSideChanges;

		int32 NumItems() const { return bExistenceConflict ? 1 : Changes.Num(); }
		int32 NumDecided() const;
		int32 NumBothChanged() const;

		/** What the merge does with the file: "saved", "deleted" or "unchanged". */
		FString Result() const;
	};

	/**
	 * -GitLevelMergeUI: all conflicted actors of one One File Per Actor level. The editor has the level open with
	 * ours in the working copy; base and theirs (and ours, for comparing) are loaded side by side, each into its own
	 * transient world, and compared property by property. Choices are applied to the live actors in the level.
	 */
	class FLevelMergeSession
	{
	public:
		~FLevelMergeSession();

		/** The level package named in a manifest, read before the editor has finished starting. */
		static FString ReadManifestLevel(const FString& ManifestFile);

		/** Reads the manifest GitO wrote, opens the level and compares every actor. */
		bool Load(const FString& ManifestFile);

		const FString& GetLevel() const { return Level; }
		const FString& GetOursLabel() const { return OursLabel; }
		const FString& GetTheirsLabel() const { return TheirsLabel; }
		TArray<FLevelActor>& GetActors() { return Actors; }
		const TArray<FString>& GetProblems() const { return Problems; }

		int32 NumItems() const;
		int32 NumDecided() const;
		bool IsAllDecided() const { return NumDecided() == NumItems(); }

		/** Applies a choice to the live actor right away, so the viewport shows the result. */
		void Choose(int32 ActorIndex, int32 ChangeIndex, ELevelChoice Choice);
		void ChooseExistence(int32 ActorIndex, ELevelChoice Choice);

		/**
		 * One choice for many items: every actor (ActorIndex INDEX_NONE) or one, every category (empty) or one.
		 * Keep / delete decisions take Ours or Theirs only; Level leaves them as they are.
		 */
		void ChooseAll(int32 ActorIndex, const FString& Category, ELevelChoice Choice);
		bool IsAllChosen(int32 ActorIndex, const FString& Category, ELevelChoice Choice) const;

		/** The property's value in the level now, in the same text form as Base / Ours / Theirs. */
		TOptional<FString> GetLiveValue(int32 ActorIndex, int32 ChangeIndex) const;

		/** Ours or Theirs was chosen, then the value was changed in the level by hand. */
		bool IsEditedInLevel(int32 ActorIndex, int32 ChangeIndex) const;

		int32 NumEditedInLevel() const;
		int32 NumChosen(ELevelChoice Choice) const;
		int32 NumResults(const TCHAR* Result) const;

		/** Unsaved packages in the editor that the merge doesn't save. */
		int32 NumOtherDirtyPackages() const;

		/** Selects the live actor and moves the viewport camera to it. */
		void Focus(int32 ActorIndex) const;

		/** Saves every kept actor; deletions are left to GitO (it removes the file and stages that). */
		bool Save();

		/** Leaves the live actors unsaved (cancel): clears their dirty flags so quitting doesn't ask. */
		void Discard();

		void WriteReport(const FString& File) const;
		bool WasWritten() const { return bWritten; }

	private:
		bool LoadActor(FLevelActor& Actor, const TSharedPtr<class FJsonObject>& Json);
		UPackage* LoadRevision(const FString& File, const TCHAR* Label, TMap<FString, FString>& OutFacts, TMap<FString, FString>& OutCategories);
		void LoadLiveActors();
		void Apply(FLevelActor& Actor, FLevelChange& Change, const FString& Value);
		bool FindLiveProperty(const FLevelActor& Actor, const FLevelChange& Change, UObject*& OutTarget, FProperty*& OutProperty) const;

		FString Level;
		FString OursLabel;
		FString TheirsLabel;
		TArray<FLevelActor> Actors;
		TArray<FString> Problems;
		TArray<FString> TempFiles;
		TArray<UPackage*> TempPackages;
		bool bWritten = false;

		/** Keeps the conflicted actors of a World Partition level loaded while the window is open. */
		TUniquePtr<FLoaderAdapterActorList> Loader;
	};
}
