// Copyright (c) MyGitGUI contributors. MIT License.

#pragma once

#include "BlueprintSnapshot.h"

class UBlueprint;
class UEdGraphNode;

namespace GitBlueprintMerge
{
	/**
	 * Applies every change the plan takes from Theirs onto Ours (Ours already holds its own changes),
	 * then compiles Ours. Only call it for plans without conflicts.
	 *
	 * Returns false and fills OutProblems when a change is not supported yet or the result does not
	 * compile. Ours may be partially modified then and must not be saved.
	 */
	bool ApplyTheirs(const FMergePlan& Plan, UBlueprint* Ours, UBlueprint* Theirs, TArray<FString>& OutProblems);

	/** Compiles a merged Blueprint; false with the compile errors in OutProblems when it doesn't compile. */
	bool CompileMerged(UBlueprint* Blueprint, TArray<FString>& OutProblems);

	/**
	 * Copies Nodes (from Source, another revision) into the same graphs of Target, with references to Source's
	 * package pointed at Target's. Nodes Target already has get a new id. Wires between the copies are kept; wires to
	 * nodes outside the copy are made again to the same nodes in Target, unless that would replace a wire already there.
	 * With DropAt (graph coordinates), the copies are moved so their top-left corner lands there.
	 * Returns the copies; OutProblem says why some couldn't be copied (e.g. their graph isn't in Target).
	 */
	TArray<UEdGraphNode*> CopyNodes(const TArray<UEdGraphNode*>& Nodes, UBlueprint* Source, UBlueprint* Target, FString& OutProblem, const FVector2D* DropAt = nullptr);
}
