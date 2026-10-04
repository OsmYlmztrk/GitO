// Copyright (c) MyGitGUI contributors. MIT License.

#pragma once

#include "BlueprintSnapshot.h"

class UBlueprint;

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
}
