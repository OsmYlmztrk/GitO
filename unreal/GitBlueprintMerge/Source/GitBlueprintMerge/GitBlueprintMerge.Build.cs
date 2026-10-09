// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

using UnrealBuildTool;

public class GitBlueprintMerge : ModuleRules
{
	public GitBlueprintMerge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"BlueprintGraph",
			"Json",
			"Slate",
			"SlateCore",
			"InputCore",
			"MainFrame",
			"GraphEditor",
			"Kismet",
			"PropertyEditor",
			"UMG",
		});
	}
}
