// Copyright (c) MyGitGUI contributors. MIT License.

#pragma once

#include "Commandlets/Commandlet.h"

#include "GitBlueprintMergeTestCaseCommandlet.generated.h"

/**
 * Development helper: makes Ours/Theirs edits of a base Blueprint that conflict in known ways, so the
 * merge can be tested without editing Blueprints by hand. Built for the MergeTest/BP_Test asset
 * (BeginPlay -> Print String -> ... chain, Event Tick).
 *
 *   UnrealEditor-Cmd.exe Project.uproject -run=GitBlueprintMergeTestCase
 *       -Base=<file> -OutDir=<dir> -PackageName=/Game/MergeTest/BP_Test
 *
 * Writes <OutDir>/Ours.uasset and <OutDir>/Theirs.uasset.
 *
 *   -run=GitBlueprintMergeTestCase -DataFixtures -OutDir=<dir>
 *       Writes DA_/DT_/MI_Fixture_Base/_Ours/_Theirs.uasset: a data asset, a data table and a material instance.
 *   -run=GitBlueprintMergeTestCase -BlueprintFixture -OutDir=<dir>
 *       Writes BP_Fixture_Base/_Ours/_Theirs.uasset: an actor Blueprint changed in its graphs, variables, class
 *       defaults and components, with one conflict in each.
 *   -run=GitBlueprintMergeTestCase -Dump=<file>
 *       Logs every fact of an asset ("FACT key = value"), to check a merged result.
 */
UCLASS()
class UGitBlueprintMergeTestCaseCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGitBlueprintMergeTestCaseCommandlet();

	virtual int32 Main(const FString& Params) override;
};
