// Copyright (c) MyGitGUI contributors. MIT License.

#include "GitBlueprintMergeTestCaseCommandlet.h"

#include "DataTableEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MergeSession.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GitBlueprintMergeTestCaseCommandlet)

DEFINE_LOG_CATEGORY_STATIC(LogGitBlueprintMergeTestCase, Log, All);

namespace
{
	/** First node with this title (spaces ignored), optionally with a pin holding the given default value. */
	UEdGraphNode* FindNode(const UBlueprint* Blueprint, const TCHAR* Title, const TCHAR* PinName = nullptr, const TCHAR* PinValue = nullptr)
	{
		const FString Wanted = FString(Title).Replace(TEXT(" "), TEXT(""));

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Replace(TEXT(" "), TEXT("")) != Wanted)
					continue;

				const UEdGraphPin* Pin = PinName ? Node->FindPin(FName(PinName)) : nullptr;
				if (!PinName || (Pin && Pin->DefaultValue == PinValue))
					return Node;
			}
		}

		UE_LOG(LogGitBlueprintMergeTestCase, Error, TEXT("Node '%s' not found."), Title);
		return nullptr;
	}

	/** Sets a property of Container (an object or a row of Struct) from text. */
	void SetText(const UStruct* Struct, void* Container, UObject* Owner, const TCHAR* Name, const TCHAR* Value)
	{
		const FProperty* Property = FindFProperty<FProperty>(Struct, FName(Name));
		if (!Property || !Property->ImportText_InContainer(Value, Container, Owner, PPF_None))
			UE_LOG(LogGitBlueprintMergeTestCase, Error, TEXT("Could not set '%s' to '%s'."), Name, Value);
	}

	void SetText(UObject* Object, const TCHAR* Name, const TCHAR* Value)
	{
		SetText(Object->GetClass(), Object, Object, Name, Value);
	}

	void SetRow(UDataTable* Table, const TCHAR* Row, const TCHAR* Field, const TCHAR* Value)
	{
		uint8* Memory = Table->FindRowUnchecked(FName(Row));
		if (!Memory)
			Memory = FDataTableEditorUtils::AddRow(Table, FName(Row));
		SetText(Table->GetRowStruct(), Memory, Table, Field, Value);
	}

	bool SaveAsset(UObject* Asset, const FString& File)
	{
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Standalone;
		Args.Error = GLog;
		return UPackage::Save(Asset->GetPackage(), Asset, *File, Args).Result == ESavePackageResult::Success;
	}

	/**
	 * -DataFixtures: a data asset, a data table and a material instance, each as <Name>_Base/_Ours/_Theirs with
	 * clean changes on both sides and one conflict.
	 */
	int32 WriteDataFixtures(const FString& OutDir)
	{
		using namespace GitBlueprintMerge;

		struct FFixture
		{
			const TCHAR* Name;
			TFunction<UObject*(UPackage*)> MakeBase;
			TFunction<void(UObject*, bool)> Edit;
		};

		const FMaterialParameterInfo Roughness(TEXT("Roughness"));
		const FMaterialParameterInfo Metallic(TEXT("Metallic"));
		const FMaterialParameterInfo Tint(TEXT("Tint"));
		const FMaterialParameterInfo UseDetail(TEXT("UseDetail"));

		TArray<FFixture> Fixtures;
		Fixtures.Add({
			TEXT("DA_Fixture"),
			[](UPackage* Package) -> UObject*
			{
				UDataLayerAsset* Asset = NewObject<UDataLayerAsset>(Package, TEXT("DA_Fixture"), RF_Public | RF_Standalone);
				SetText(Asset, TEXT("DebugColor"), TEXT("(B=10,G=10,R=10,A=255)"));
				return Asset;
			},
			[](UObject* Asset, bool bOurs)
			{
				// Clean: different fields of the same struct, and different properties. Conflict: LoadFilter.
				SetText(Asset, TEXT("DebugColor"), bOurs ? TEXT("(B=10,G=10,R=255,A=255)") : TEXT("(B=10,G=200,R=10,A=255)"));
				SetText(Asset, bOurs ? TEXT("DataLayerType") : TEXT("bSupportsActorFilters"), bOurs ? TEXT("Editor") : TEXT("True"));
				SetText(Asset, TEXT("LoadFilter"), bOurs ? TEXT("ClientOnly") : TEXT("ServerOnly"));
			},
		});
		Fixtures.Add({
			TEXT("DT_Fixture"),
			[](UPackage* Package) -> UObject*
			{
				UDataTable* Table = NewObject<UDataTable>(Package, TEXT("DT_Fixture"), RF_Public | RF_Standalone);
				Table->RowStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/GameplayTags.GameplayTagTableRow"));
				for (const TCHAR* Row : {TEXT("A"), TEXT("B"), TEXT("C")})
				{
					SetRow(Table, Row, TEXT("Tag"), *FString::Printf(TEXT("Fixture.%s"), Row));
					SetRow(Table, Row, TEXT("DevComment"), TEXT("base"));
				}
				return Table;
			},
			[](UObject* Asset, bool bOurs)
			{
				UDataTable* Table = CastChecked<UDataTable>(Asset);
				if (bOurs)
				{
					SetRow(Table, TEXT("A"), TEXT("DevComment"), TEXT("ours"));
					FDataTableEditorUtils::RemoveRow(Table, TEXT("C"));
					SetRow(Table, TEXT("D"), TEXT("Tag"), TEXT("Fixture.D"));
				}
				else
				{
					// Clean: another field of A, row B, a new row E. Conflict: C, which ours removed.
					SetRow(Table, TEXT("A"), TEXT("Tag"), TEXT("Fixture.A2"));
					SetRow(Table, TEXT("B"), TEXT("DevComment"), TEXT("theirs"));
					SetRow(Table, TEXT("C"), TEXT("DevComment"), TEXT("theirs"));
					SetRow(Table, TEXT("E"), TEXT("Tag"), TEXT("Fixture.E"));
				}
			},
		});
		Fixtures.Add({
			TEXT("MI_Fixture"),
			[Roughness](UPackage* Package) -> UObject*
			{
				UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(Package, TEXT("MI_Fixture"), RF_Public | RF_Standalone);
				Instance->SetParentEditorOnly(LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial")));
				Instance->SetScalarParameterValueEditorOnly(Roughness, 0.5f);
				Instance->PostEditChange();
				return Instance;
			},
			[=](UObject* Asset, bool bOurs)
			{
				UMaterialInstanceConstant* Instance = CastChecked<UMaterialInstanceConstant>(Asset);
				if (bOurs)
				{
					// Conflict: Roughness on both sides. Clean: a new vector parameter.
					Instance->SetScalarParameterValueEditorOnly(Roughness, 0.7f);
					Instance->SetVectorParameterValueEditorOnly(Tint, FLinearColor(1.0f, 0.0f, 0.0f));
				}
				else
				{
					// Clean: a new scalar, a static switch, a base property override.
					Instance->SetScalarParameterValueEditorOnly(Roughness, 0.2f);
					Instance->SetScalarParameterValueEditorOnly(Metallic, 1.0f);
					Instance->SetStaticSwitchParameterValueEditorOnly(UseDetail, true);
					Instance->BasePropertyOverrides.bOverride_TwoSided = true;
					Instance->BasePropertyOverrides.TwoSided = true;
				}
				Instance->PostEditChange();
			},
		});

		for (const FFixture& Fixture : Fixtures)
		{
			const FString PackageName = FString(TEXT("/Game/GBMFixtures/")) + Fixture.Name;
			const FString BaseFile = FPaths::Combine(OutDir, FString(Fixture.Name) + TEXT("_Base.uasset"));
			UObject* Base = Fixture.MakeBase(CreatePackage(*PackageName));
			if (!SaveAsset(Base, BaseFile))
				return 2;
			Base->GetPackage()->Rename(*FString::Printf(TEXT("/Temp/GBM_FixtureBase_%s"), *FGuid::NewGuid().ToString()), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);

			for (const bool bOurs : {true, false})
			{
				const FString OutFile = FPaths::Combine(OutDir, FString(Fixture.Name) + (bOurs ? TEXT("_Ours.uasset") : TEXT("_Theirs.uasset")));
				FRevision Revision;
				if (!LoadRevision(BaseFile, bOurs ? TEXT("Ours") : TEXT("Theirs"), Revision))
					return 2;
				RenameRevision(Revision, PackageName, OutFile);
				Fixture.Edit(Revision.Asset, bOurs);

				TArray<FString> Problems;
				const bool bSaved = SaveRevision(Revision, OutFile, Problems);
				Revision.Package->Rename(*FString::Printf(TEXT("/Temp/GBM_FixtureDone_%s"), *FGuid::NewGuid().ToString()), nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
				ReleaseRevision(Revision);
				if (!bSaved)
					return 2;
			}
			UE_LOG(LogGitBlueprintMergeTestCase, Display, TEXT("Wrote %s_Base/_Ours/_Theirs"), Fixture.Name);
		}
		return 0;
	}

	/** -Dump=<file>: every fact of the asset, to check a merged result. */
	int32 DumpAsset(const FString& File)
	{
		using namespace GitBlueprintMerge;

		FRevision Revision;
		if (!LoadRevision(File, TEXT("Dump"), Revision))
			return 2;

		const FSnapshot Snap = TakeAssetSnapshot(Revision.Asset);
		TArray<FString> Keys;
		Snap.Facts.GetKeys(Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
			UE_LOG(LogGitBlueprintMergeTestCase, Display, TEXT("FACT %s = %s"), *Key, *Snap.Facts[Key]);
		ReleaseRevision(Revision);
		return 0;
	}
}


UGitBlueprintMergeTestCaseCommandlet::UGitBlueprintMergeTestCaseCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false;
}

int32 UGitBlueprintMergeTestCaseCommandlet::Main(const FString& Params)
{
	using namespace GitBlueprintMerge;

	FString BaseFile, OutDir, PackageName, DumpFile;
	FParse::Value(*Params, TEXT("Base="), BaseFile);
	FParse::Value(*Params, TEXT("OutDir="), OutDir);
	FParse::Value(*Params, TEXT("PackageName="), PackageName);

	if (FParse::Value(*Params, TEXT("Dump="), DumpFile))
		return DumpAsset(DumpFile);
	if (FParse::Param(*Params, TEXT("DataFixtures")) && !OutDir.IsEmpty())
		return WriteDataFixtures(OutDir);

	if (BaseFile.IsEmpty() || OutDir.IsEmpty())
	{
		UE_LOG(LogGitBlueprintMergeTestCase, Error, TEXT("Usage: -run=GitBlueprintMergeTestCase -Base=<file> -OutDir=<dir> [-PackageName=<name>]"));
		return 2;
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	for (const bool bOurs : {true, false})
	{
		const TCHAR* Side = bOurs ? TEXT("Ours") : TEXT("Theirs");
		const FString OutFile = FPaths::Combine(OutDir, FString(Side) + TEXT(".uasset"));

		FRevision Revision;
		if (!LoadRevision(BaseFile, Side, Revision))
			return 2;
		RenameRevision(Revision, PackageName, OutFile);

		UBlueprint* Blueprint = Revision.Blueprint;
		UEdGraphNode* PrintBase = FindNode(Blueprint, TEXT("Print String"), TEXT("InString"), TEXT("Base: BeginPlay"));
		UEdGraphNode* PrintAbove = FindNode(Blueprint, TEXT("Print String"), TEXT("InString"), TEXT("Above ground"));
		UEdGraphNode* Literal = FindNode(Blueprint, TEXT("Make Literal Int"));
		UEdGraphNode* Branch = FindNode(Blueprint, TEXT("Branch"));
		UEdGraphNode* BreakVector = FindNode(Blueprint, TEXT("Break Vector"));
		UEdGraphNode* Tick = FindNode(Blueprint, TEXT("Event Tick"));
		if (!PrintBase || !PrintAbove || !Literal || !Branch || !BreakVector || !Tick)
		{
			ReleaseRevision(Revision);
			return 2;
		}

		if (bOurs)
		{
			// Conflict: same pin, different values.
			Schema->TrySetDefaultValue(*PrintBase->FindPin(FName(TEXT("InString"))), TEXT("Ours: BeginPlay"));
			// Conflict: ours removes a node theirs edits.
			FBlueprintEditorUtils::RemoveNode(Blueprint, Literal, true);
			// Conflict: Tick.then takes one link; each side connects it elsewhere.
			Schema->TryCreateConnection(Tick->FindPin(UEdGraphSchema_K2::PN_Then), PrintAbove->FindPin(UEdGraphSchema_K2::PN_Execute));
			// Clean: only ours comments the Branch node.
			Branch->NodeComment = TEXT("Checked by ours");
			Branch->bCommentBubbleVisible = true;
		}
		else
		{
			Schema->TrySetDefaultValue(*PrintBase->FindPin(FName(TEXT("InString"))), TEXT("Theirs: BeginPlay"));
			Schema->TrySetDefaultValue(*Literal->FindPin(FName(TEXT("Value"))), TEXT("5"));
			Schema->TryCreateConnection(Tick->FindPin(UEdGraphSchema_K2::PN_Then), PrintBase->FindPin(UEdGraphSchema_K2::PN_Execute));
			// Clean: only theirs moves Break Vector.
			BreakVector->NodePosX += 160;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

		TArray<FString> Problems;
		const bool bSaved = SaveRevision(Revision, OutFile, Problems);
		ReleaseRevision(Revision);
		if (!bSaved)
			return 2;

		UE_LOG(LogGitBlueprintMergeTestCase, Display, TEXT("Wrote %s"), *OutFile);
	}

	return 0;
}
