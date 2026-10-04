// Copyright (c) MyGitGUI contributors. MIT License.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UEdGraphPin;

namespace GitBlueprintMerge
{
	/**
	 * A Blueprint flattened into comparable facts (key -> value). Keys are built from the stable
	 * GUIDs Unreal saves with graphs, nodes, variables and components, so the same element can be
	 * matched across Base / Ours / Theirs:
	 *
	 *   parent                          parent class path
	 *   interface/<path>                implemented interface
	 *   graph/<graph>                   graph name | function|macro|event|delegate|sub
	 *   node/<node>                     node class | owning graph   (existence)
	 *   node/<node>/title|pos|comment|enabled
	 *   pin/<node>:<in|out>:<pin>       pin default value
	 *   link/<node>:out:<pin>><node>:in:<pin>
	 *   var/<var>, var/<var>/default    member variable
	 *   component/<var>, component/<var>/prop/<name>
	 *   cdo/<property>                  class default
	 */
	struct FSnapshot
	{
		TMap<FString, FString> Facts;

		/** Pins that hold a single link (exec outputs, data inputs), as "<node>:<in|out>:<pin>". */
		TSet<FString> SingleLinkPins;

		/** Input pins that are connected; their typed-in value is not used. */
		TSet<FString> LinkedInputPins;

		/** Get/Set nodes of member variables: node guid -> variable guid. */
		TMap<FString, FString> NodeVariables;

		/** Readable names for nodes, graphs, variables and components, keyed by guid; for data assets, by fact key. */
		TMap<FString, FString> Labels;

		/** Facts that are compared but can't be taken from theirs yet (see AssetMerge.h). */
		TSet<FString> Unmergeable;
	};

	FSnapshot TakeSnapshot(const UBlueprint* Blueprint);

	enum class EResolution : uint8
	{
		Unchanged,
		Both,     // both sides made the same change; as a conflict choice: keep both, ours first
		Ours,     // only ours changed it -> take ours
		Theirs,   // only theirs changed it -> take theirs
		Conflict,
		BothTheirsFirst, // only as a conflict choice: keep both, theirs first
	};

	struct FDecision
	{
		FString Key;
		EResolution Resolution = EResolution::Unchanged;
		TOptional<FString> Base;
		TOptional<FString> Ours;
		TOptional<FString> Theirs;
		FString Reason;

		/** Set when the fact only belongs to a node/variable/component that was added or removed as a whole. */
		bool bPartOfWhole = false;

		/**
		 * Conflicts that must be decided together share a group (e.g. a removed node and the edits made to it,
		 * or two links into a pin that takes one). INDEX_NONE for non-conflicts.
		 */
		int32 Group = INDEX_NONE;
	};

	struct FMergePlan
	{
		TArray<FDecision> Decisions;
		int32 NumGroups = 0;
		int32 NumOurs = 0;
		int32 NumTheirs = 0;
		int32 NumBoth = 0;
		int32 NumConflicts = 0;
	};

	FMergePlan PlanMerge(const FSnapshot& Base, const FSnapshot& Ours, const FSnapshot& Theirs);

	/** Recomputes bPartOfWhole and the counters; call after changing resolutions. */
	void FinalizePlan(FMergePlan& Plan);

	/** Takes the chosen side (Ours/Theirs) for every conflict of each group. False when a group has no choice. */
	bool ResolveConflicts(FMergePlan& Plan, const TMap<int32, EResolution>& Choices);

	/** What one side put into a contested exec connection: new nodes, the link into them and the link out. */
	struct FInsertion
	{
		int32 Entry = INDEX_NONE;
		int32 Exit = INDEX_NONE;
		TArray<FString> Nodes;
	};

	/**
	 * True when both sides only put new nodes into the same exec connection (e.g. ours a Delay, theirs a
	 * Play Sound) that lead back to the same place, so both can be kept, one after the other.
	 */
	bool FindInsertions(const FMergePlan& Plan, int32 Group, const FSnapshot& Ours, const FSnapshot& Theirs, FInsertion& OutOurs, FInsertion& OutTheirs);

	/**
	 * Resolves Group by keeping both insertions, chained: the first side's nodes, then the other side's.
	 * Returns the second side's nodes (so they can be moved aside), empty when Group can't be taken both ways.
	 */
	TArray<FString> TakeBoth(FMergePlan& Plan, int32 Group, bool bOursFirst, const FSnapshot& Ours, const FSnapshot& Theirs);

	/**
	 * The decision that best names a conflict group: the node/variable/component itself when it is part of it,
	 * preferring one that existed in base (the thing one side removed) over ones a side added.
	 */
	int32 GroupHead(const FMergePlan& Plan, int32 Group);

	/** False for the noise of a conflict group: values of added nodes, and values the keeping side left alone. */
	bool IsWorthShowing(const FDecision& Decision);

	/**
	 * True when the merge can't take theirs' value for this fact yet: graphs other than functions, adding,
	 * removing or moving components, interfaces, the parent class, nodes in such graphs, and nodes moved to
	 * another graph. Ours' changes are always kept, since the result starts from ours.
	 */
	bool IsUnsupported(const FDecision& Decision, const FSnapshot& Ours, const FSnapshot& Theirs);

	/**
	 * Unsupported changes the plan would take from theirs (clean ones, and conflicts if theirs is chosen),
	 * one per thing: nodes of a graph only theirs has are covered by that graph.
	 */
	TArray<int32> FindUnsupported(const FMergePlan& Plan, const FSnapshot& Ours, const FSnapshot& Theirs);

	/** Keeps ours instead for every unsupported change taken from theirs, along with what belongs to it. */
	void LeaveOutUnsupported(FMergePlan& Plan, const FSnapshot& Ours, const FSnapshot& Theirs);

	/** The node a fact is about ("node/", "pin/", or the source node of a "link/"), or empty. */
	FString NodeIdOfKey(const FString& Key);

	/** Every node a fact touches: both ends of a link, otherwise the node of a pin/node fact. */
	TArray<FString> NodeIdsOfKey(const FString& Key);

	/** A conflict group in words: what it is about and what each side did. */
	struct FConflictSummary
	{
		FString Title;
		FString Ours;
		FString Theirs;
	};

	FConflictSummary SummarizeGroup(const FMergePlan& Plan, int32 Group, const FSnapshot& Base, const FSnapshot& Ours, const FSnapshot& Theirs);

	FString Describe(const FDecision& Decision, const FSnapshot& Base, const FSnapshot& Ours, const FSnapshot& Theirs);

	/** Facts that are not about graph nodes: variables, class defaults, components, graphs, interfaces, parent. */
	bool IsValueKey(const FString& Key);

	/** What a fact is about, e.g. "Class default 'OpenSpeed'". */
	FString DescribeSubject(const FString& Key, const FSnapshot& Base, const FSnapshot& Ours, const FSnapshot& Theirs);

	/** One version's value of a fact, to show next to that version ("Not in this version" when absent). */
	FString DescribeValue(const FString& Key, const TOptional<FString>& Value, const FSnapshot& Base, const FSnapshot& Ours, const FSnapshot& Theirs);

	const TCHAR* ToString(EResolution Resolution);

	/** The value the merge keeps: theirs when taken from theirs, ours otherwise. */
	const TOptional<FString>& MergedValue(const FDecision& Decision);

	/** "<in|out>:<pin name>" */
	FString PinKey(const UEdGraphPin* Pin);

	FString GraphId(const UEdGraph* Graph);

	/** "link/<src>|<dst>" -> source and target pin refs. */
	bool SplitLink(const FString& Key, FString& Src, FString& Dst);

	/** "<node>:<in|out>:<pin>" -> node guid and pin key ("<in|out>:<pin>"). */
	void SplitPinRef(const FString& Ref, FString& Node, FString& Pin);
}
