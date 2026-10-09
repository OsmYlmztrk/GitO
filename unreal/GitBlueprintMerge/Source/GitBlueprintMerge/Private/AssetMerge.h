// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#pragma once

#include "BlueprintSnapshot.h"

namespace GitBlueprintMerge
{
	/** What a revision holds, and so how it is compared and merged. */
	enum class EAssetKind : uint8
	{
		Unsupported,
		Blueprint,
		DataAsset,
		DataTable,
		MaterialInstance,
	};

	EAssetKind GetAssetKind(const UObject* Asset);

	/** "Blueprint", "Data Asset", ... */
	const TCHAR* AssetKindName(EAssetKind Kind);

	/**
	 * Data assets, data tables and material instances flattened into facts, merged property by property:
	 *
	 *   class                           asset class (data assets)
	 *   prop/<path>                     editable property; plain structs are split into fields (A.B), fixed arrays into A[1]
	 *   rowstruct                       data table row structure
	 *   row/<row>, row/<row>/<path>     data table row (existence) and its fields
	 *   param/<array>/<parameter>       material instance parameter override (present = overridden)
	 *   switch/<parameter>              material instance static switch override
	 *   staticother                     material layers and component masks
	 *   subobject/<name>                everything inside an instanced subobject
	 *
	 * Containers (arrays, maps, sets) are one value each. Facts in FSnapshot::Unmergeable are compared but
	 * not merged yet: the class, the row structure, layers and masks, and anything holding instanced subobjects.
	 */
	FSnapshot TakeDataSnapshot(const UObject* Asset);

	/** Snapshot of any supported asset. */
	FSnapshot TakeAssetSnapshot(const UObject* Asset);

	/** Applies what the plan takes from theirs onto ours, for the assets TakeDataSnapshot handles. */
	bool ApplyTheirsToData(const FMergePlan& Plan, UObject* Ours, UObject* Theirs, TArray<FString>& OutProblems);
}
