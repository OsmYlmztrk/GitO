// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#pragma once

#include "Commandlets/Commandlet.h"

#include "GitBlueprintMergeCommandlet.generated.h"

/**
 * Three-way merge for Blueprint assets.
 *
 *   UnrealEditor-Cmd.exe Project.uproject -run=GitBlueprintMerge
 *       -Base=<file> -Ours=<file> -Theirs=<file> [-Report=<json>]
 *       [-Output=<file> [-PackageName=/Game/Path/Asset]]
 *
 * Without -Output it only reports. With -Output it writes the merged asset when there are no conflicts.
 * Exit code: 0 = clean (and written), 1 = has conflicts, 2 = error, 3 = could not apply. The JSON
 * report is the source of truth.
 */
UCLASS()
class UGitBlueprintMergeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGitBlueprintMergeCommandlet();

	virtual int32 Main(const FString& Params) override;
};
