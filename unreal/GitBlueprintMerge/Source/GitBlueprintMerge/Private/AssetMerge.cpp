// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#include "AssetMerge.h"

#include "DataTableEditorUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "StaticParameterSet.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectHash.h"

namespace GitBlueprintMerge
{
	EAssetKind GetAssetKind(const UObject* Asset)
	{
		if (!Asset)
			return EAssetKind::Unsupported;
		if (Asset->IsA<UBlueprint>())
			return EAssetKind::Blueprint;
		if (Asset->IsA<UDataAsset>())
			return EAssetKind::DataAsset;
		if (Asset->IsA<UDataTable>())
			return EAssetKind::DataTable;
		if (Asset->IsA<UMaterialInstanceConstant>())
			return EAssetKind::MaterialInstance;
		return EAssetKind::Unsupported;
	}

	const TCHAR* AssetKindName(EAssetKind Kind)
	{
		switch (Kind)
		{
		case EAssetKind::Blueprint: return TEXT("Blueprint");
		case EAssetKind::DataAsset: return TEXT("Data Asset");
		case EAssetKind::DataTable: return TEXT("Data Table");
		case EAssetKind::MaterialInstance: return TEXT("Material Instance");
		default: return TEXT("Asset");
		}
	}

	namespace
	{
		/** Structs nested deeper than this are compared as one value. */
		constexpr int32 MaxDepth = 4;

		/** The three revisions live in differently named temp packages; hide that so values compare equal. */
		struct FNormalizer
		{
			FString PackageName;
			FString ShortName;

			explicit FNormalizer(const UObject* Asset)
				: PackageName(Asset->GetPackage()->GetName())
				, ShortName(FPackageName::GetShortName(PackageName))
			{
			}

			FString operator()(const FString& Value) const
			{
				return Value.Replace(*PackageName, TEXT("<self>")).Replace(*ShortName, TEXT("<self>"));
			}
		};

		/** Rewrites references to Theirs' temp package so they point at Ours. */
		struct FRetarget
		{
			FString From;
			FString To;
			FString FromShort;
			FString ToShort;

			FRetarget(const UObject* Theirs, const UObject* Ours)
				: From(Theirs->GetPackage()->GetName())
				, To(Ours->GetPackage()->GetName())
				, FromShort(FPackageName::GetShortName(From))
				, ToShort(FPackageName::GetShortName(To))
			{
			}

			FString operator()(const FString& Text) const
			{
				return Text.Replace(*From, *To).Replace(*FromShort, *ToShort);
			}
		};

		/** Row names become part of keys, which use '/' as a separator. */
		FString EscapeRow(const FString& Name)
		{
			return Name.Replace(TEXT("%"), TEXT("%25")).Replace(TEXT("/"), TEXT("%2F"));
		}

		FString UnescapeRow(const FString& Id)
		{
			return Id.Replace(TEXT("%2F"), TEXT("/")).Replace(TEXT("%25"), TEXT("%"));
		}

		bool IsSkipped(const FProperty* Property)
		{
			return Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated);
		}

		bool HoldsInstancedObjects(const FProperty* Property)
		{
			return Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance);
		}

		/**
		 * Structs merged field by field: plain ones whose every field is editable and whose text is the default
		 * (Name=Value,...) form. Others (e.g. tag containers with hidden bookkeeping) stay one value.
		 */
		const UScriptStruct* SplittableStruct(const FProperty* Property)
		{
			const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty || !StructProperty->Struct)
				return nullptr;

			const UScriptStruct* Struct = StructProperty->Struct;
			if (Struct->StructFlags & (STRUCT_ImportTextItemNative | STRUCT_ExportTextItemNative))
				return nullptr;

			bool bAny = false;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (IsSkipped(*It))
					continue;
				if (!It->HasAnyPropertyFlags(CPF_Edit))
					return nullptr;
				bAny = true;
			}
			return bAny ? Struct : nullptr;
		}

		/** Adds one property (Key / Label) of Container, split into its fields when it is a plain struct. */
		void AddValues(FSnapshot& Snap, const FString& Key, const FString& Label, const FProperty* Property, const void* Container, const FNormalizer& Normalize, int32 Depth = 0)
		{
			for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
			{
				const FString ItemKey = Property->ArrayDim == 1 ? Key : FString::Printf(TEXT("%s[%d]"), *Key, Index);
				const FString ItemLabel = Property->ArrayDim == 1 ? Label : FString::Printf(TEXT("%s[%d]"), *Label, Index);
				const void* Value = Property->ContainerPtrToValuePtr<void>(Container, Index);

				if (const UScriptStruct* Struct = Depth < MaxDepth ? SplittableStruct(Property) : nullptr)
				{
					for (TFieldIterator<FProperty> It(Struct); It; ++It)
					{
						if (!IsSkipped(*It))
							AddValues(Snap, ItemKey + TEXT(".") + It->GetName(), ItemLabel + TEXT(".") + It->GetAuthoredName(), *It, Value, Normalize, Depth + 1);
					}
					continue;
				}

				FString Text;
				Property->ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
				Snap.Facts.Add(ItemKey, Normalize(Text));
				Snap.Labels.Add(ItemKey, ItemLabel);
				if (HoldsInstancedObjects(Property))
					Snap.Unmergeable.Add(ItemKey);
			}
		}

		/** Walks "A.B[1].C" from Container (an object or a row) to the property and the memory of its value. */
		bool FindValue(const UStruct* Struct, void* Container, const FString& Path, const FProperty*& OutProperty, void*& OutValue)
		{
			TArray<FString> Parts;
			Path.ParseIntoArray(Parts, TEXT("."));

			const UStruct* Current = Struct;
			void* Memory = Container;
			for (int32 Part = 0; Part < Parts.Num(); ++Part)
			{
				FString Name = Parts[Part];
				int32 Index = 0, Bracket = INDEX_NONE;
				if (Name.FindChar(TEXT('['), Bracket))
				{
					Index = FCString::Atoi(*Name.Mid(Bracket + 1));
					Name.LeftInline(Bracket);
				}

				const FProperty* Property = FindFProperty<FProperty>(Current, FName(*Name));
				if (!Property || Index < 0 || Index >= Property->ArrayDim)
					return false;

				void* Value = Property->ContainerPtrToValuePtr<void>(Memory, Index);
				if (Part == Parts.Num() - 1)
				{
					OutProperty = Property;
					OutValue = Value;
					return true;
				}

				const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
				if (!StructProperty)
					return false;
				Current = StructProperty->Struct;
				Memory = Value;
			}
			return false;
		}

		/** Copies one value from theirs into ours through text, so references to theirs' package point at ours. */
		bool CopyValue(const FProperty* Mine, void* MyValue, UObject* MyOwner, const FProperty* Their, const void* TheirValue, const FRetarget& Retarget)
		{
			FString Text;
			Their->ExportTextItem_Direct(Text, TheirValue, nullptr, nullptr, PPF_None);
			return Mine->ImportText_Direct(*Retarget(Text), MyValue, MyOwner, PPF_None) != nullptr;
		}

		/** Arrays of material parameter overrides (ScalarParameterValues, ...): their elements carry a ParameterInfo. */
		const FStructProperty* ParameterInfoOf(const FProperty* Property)
		{
			const FArrayProperty* Array = CastField<FArrayProperty>(Property);
			const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
			const FStructProperty* Info = Inner ? CastField<FStructProperty>(Inner->Struct->FindPropertyByName(TEXT("ParameterInfo"))) : nullptr;
			return Info && Info->Struct == FMaterialParameterInfo::StaticStruct() ? Info : nullptr;
		}

		/** "Roughness", or "Roughness@Layer1" for a parameter of a material layer. */
		FString ParameterId(const FMaterialParameterInfo& Info)
		{
			const FString Name = EscapeRow(Info.Name.ToString());
			switch (Info.Association)
			{
			case LayerParameter: return FString::Printf(TEXT("%s@Layer%d"), *Name, Info.Index);
			case BlendParameter: return FString::Printf(TEXT("%s@Blend%d"), *Name, Info.Index);
			default: return Name;
			}
		}

		FString ParameterLabel(const FString& Kind, const FMaterialParameterInfo& Info)
		{
			const FString Name = Info.Name.ToString();
			switch (Info.Association)
			{
			case LayerParameter: return FString::Printf(TEXT("%s '%s' (layer %d)"), *Kind, *Name, Info.Index);
			case BlendParameter: return FString::Printf(TEXT("%s '%s' (blend %d)"), *Kind, *Name, Info.Index);
			default: return FString::Printf(TEXT("%s '%s'"), *Kind, *Name);
			}
		}

		/** "Scalar Parameter Values" -> "Scalar parameter". */
		FString ParameterKind(const FProperty* Array)
		{
			FString Kind = Array->GetDisplayNameText().ToString();
			Kind.RemoveFromEnd(TEXT(" Values"));
			return Kind.Left(1) + Kind.Mid(1).ToLower();
		}

		int32 FindParameter(const FArrayProperty* Array, const FStructProperty* InfoProperty, void* ArrayMemory, const FString& Id)
		{
			FScriptArrayHelper Helper(Array, ArrayMemory);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				const FMaterialParameterInfo& Info = *InfoProperty->ContainerPtrToValuePtr<FMaterialParameterInfo>(Helper.GetRawPtr(Index));
				if (ParameterId(Info) == Id)
					return Index;
			}
			return INDEX_NONE;
		}

		/** The parameter's value as people read it: its editable fields other than ParameterInfo. */
		FString ParameterValue(const UScriptStruct* Struct, const void* Element)
		{
			TArray<FString> Names, Values;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_Edit) || IsSkipped(*It) || It->GetFName() == TEXT("ParameterInfo"))
					continue;
				FString Text;
				It->ExportTextItem_InContainer(Text, Element, nullptr, nullptr, PPF_None);
				Names.Add(It->GetName());
				Values.Add(Text);
			}
			if (Values.Num() == 1)
				return Values[0];

			TArray<FString> Fields;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
				Fields.Add(Names[Index] + TEXT("=") + Values[Index]);
			return FString::Join(Fields, TEXT(", "));
		}

		void AddObjectProperties(FSnapshot& Snap, const UObject* Object, const FNormalizer& Normalize, TFunctionRef<bool(const FProperty*)> Skip)
		{
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_Edit) || IsSkipped(*It) || Skip(*It))
					continue;
				AddValues(Snap, TEXT("prop/") + It->GetName(), It->GetAuthoredName(), *It, Object, Normalize);
			}
		}

		/** Instanced subobjects (e.g. EditInlineNew objects in a data asset): compared as a whole, not merged yet. */
		void AddSubobjects(FSnapshot& Snap, const UObject* Asset, const FNormalizer& Normalize)
		{
			TArray<UObject*> Inner;
			GetObjectsWithOuter(Asset, Inner, EGetObjectsFlags::IncludeNestedObjects);
			for (const UObject* Sub : Inner)
			{
				if (!Sub || Sub->HasAnyFlags(RF_Transient) || Sub->GetClass()->GetName().Contains(TEXT("ThumbnailInfo")))
					continue;

				FString Dump = Sub->GetClass()->GetPathName();
				for (TFieldIterator<FProperty> It(Sub->GetClass()); It; ++It)
				{
					if (!It->HasAnyPropertyFlags(CPF_Edit) || IsSkipped(*It))
						continue;
					FString Text;
					It->ExportTextItem_InContainer(Text, Sub, nullptr, nullptr, PPF_None);
					Dump += TEXT("\n") + It->GetName() + TEXT("=") + Text;
				}

				const FString Name = Sub->GetPathName(Asset);
				const FString Key = TEXT("subobject/") + EscapeRow(Name);
				Snap.Facts.Add(Key, Normalize(Dump));
				Snap.Labels.Add(Key, FString::Printf(TEXT("Subobject '%s'"), *Name));
				Snap.Unmergeable.Add(Key);
			}
		}

		void SnapshotDataTable(FSnapshot& Snap, const UDataTable* Table, const FNormalizer& Normalize)
		{
			const UScriptStruct* RowStruct = Table->GetRowStruct();
			Snap.Facts.Add(TEXT("rowstruct"), GetPathNameSafe(RowStruct));
			Snap.Labels.Add(TEXT("rowstruct"), TEXT("Row structure"));
			Snap.Unmergeable.Add(TEXT("rowstruct"));
			if (!RowStruct)
				return;

			for (const TPair<FName, uint8*>& Row : Table->GetRowMap())
			{
				const FString Name = Row.Key.ToString();
				const FString Key = TEXT("row/") + EscapeRow(Name);
				Snap.Facts.Add(Key, TEXT("1"));
				Snap.Labels.Add(Key, Name);

				for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
				{
					if (!IsSkipped(*It))
						AddValues(Snap, Key + TEXT("/") + It->GetName(), It->GetAuthoredName(), *It, Row.Value, Normalize);
				}
			}
		}

		void SnapshotMaterialInstance(FSnapshot& Snap, const UMaterialInstanceConstant* Instance, const FNormalizer& Normalize)
		{
			// Plain settings (parent, physical material, base property overrides, ...).
			AddObjectProperties(Snap, Instance, Normalize, [](const FProperty* Property) { return ParameterInfoOf(Property) != nullptr; });

			// Parameter overrides, one fact each: present = overridden in this instance.
			for (TFieldIterator<FProperty> It(Instance->GetClass()); It; ++It)
			{
				const FStructProperty* InfoProperty = ParameterInfoOf(*It);
				if (!InfoProperty)
					continue;

				const FArrayProperty* Array = CastFieldChecked<FArrayProperty>(*It);
				const UScriptStruct* Element = CastFieldChecked<FStructProperty>(Array->Inner)->Struct;
				const FString Kind = ParameterKind(Array);
				FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Instance));
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					const void* Memory = Helper.GetRawPtr(Index);
					const FMaterialParameterInfo& Info = *InfoProperty->ContainerPtrToValuePtr<FMaterialParameterInfo>(Memory);
					const FString Key = FString::Printf(TEXT("param/%s/%s"), *Array->GetName(), *ParameterId(Info));
					Snap.Facts.Add(Key, Normalize(ParameterValue(Element, Memory)));
					Snap.Labels.Add(Key, ParameterLabel(Kind, Info));
				}
			}

			const FStaticParameterSet Static = Instance->GetStaticParameters();
			for (const FStaticSwitchParameter& Switch : Static.StaticSwitchParameters)
			{
				if (!Switch.bOverride)
					continue;
				const FString Key = TEXT("switch/") + ParameterId(Switch.ParameterInfo);
				Snap.Facts.Add(Key, Switch.Value ? TEXT("True") : TEXT("False"));
				Snap.Labels.Add(Key, ParameterLabel(TEXT("Static switch"), Switch.ParameterInfo));
			}

			// The rest of the static parameters: compared, not merged yet.
			FString Other;
			for (TFieldIterator<FProperty> It(FStaticParameterSet::StaticStruct()); It; ++It)
			{
				if (It->GetFName() == GET_MEMBER_NAME_CHECKED(FStaticParameterSetRuntimeData, StaticSwitchParameters))
					continue;
				FString Text;
				It->ExportTextItem_InContainer(Text, &Static, nullptr, nullptr, PPF_None);
				Other += It->GetName() + TEXT("=") + Text + TEXT("\n");
			}
			Snap.Facts.Add(TEXT("staticother"), Normalize(Other));
			Snap.Labels.Add(TEXT("staticother"), TEXT("Material layers and static component masks"));
			Snap.Unmergeable.Add(TEXT("staticother"));
		}

		/** "prop/<path>" taken from theirs. */
		void ApplyProperty(const FDecision& Decision, UObject* Ours, UObject* Theirs, const FRetarget& Retarget, TArray<FString>& OutProblems)
		{
			const FString Path = Decision.Key.Mid(5);
			const FProperty* MyProperty = nullptr;
			const FProperty* TheirProperty = nullptr;
			void* MyValue = nullptr;
			void* TheirValue = nullptr;
			if (!FindValue(Ours->GetClass(), Ours, Path, MyProperty, MyValue) || !FindValue(Theirs->GetClass(), Theirs, Path, TheirProperty, TheirValue)
				|| !CopyValue(MyProperty, MyValue, Ours, TheirProperty, TheirValue, Retarget))
				OutProblems.Add(FString::Printf(TEXT("Could not set property '%s'."), *Path));
		}

		bool ApplyDataTable(const TArray<const FDecision*>& Taken, UDataTable* Ours, UDataTable* Theirs, const FRetarget& Retarget, TArray<FString>& OutProblems)
		{
			const UScriptStruct* RowStruct = Ours->GetRowStruct();
			if (!RowStruct || RowStruct != Theirs->GetRowStruct())
			{
				OutProblems.Add(TEXT("The row structure differs between the versions."));
				return false;
			}

			Ours->Modify();

			// Whole rows first, so field edits find the rows theirs added.
			for (const FDecision* Decision : Taken)
			{
				FString Id, Field;
				if (!Decision->Key.StartsWith(TEXT("row/")) || Decision->Key.Mid(4).Split(TEXT("/"), &Id, &Field))
					continue;

				const FName Name(*UnescapeRow(Decision->Key.Mid(4)));
				if (!Decision->Theirs.IsSet())
				{
					FDataTableEditorUtils::RemoveRow(Ours, Name);
					continue;
				}

				const uint8* TheirRow = Theirs->FindRowUnchecked(Name);
				uint8* MyRow = Ours->FindRowUnchecked(Name);
				if (!MyRow)
					MyRow = FDataTableEditorUtils::AddRow(Ours, Name);
				if (!TheirRow || !MyRow)
				{
					OutProblems.Add(FString::Printf(TEXT("Could not add row '%s'."), *Name.ToString()));
					continue;
				}

				FString Text;
				RowStruct->ExportText(Text, TheirRow, nullptr, Theirs, PPF_None, nullptr);
				RowStruct->ImportText(*Retarget(Text), MyRow, Ours, PPF_None, GWarn, RowStruct->GetName());
			}

			for (const FDecision* Decision : Taken)
			{
				FString Id, Field;
				if (!Decision->Key.StartsWith(TEXT("row/")) || !Decision->Key.Mid(4).Split(TEXT("/"), &Id, &Field))
					continue;

				const FName Name(*UnescapeRow(Id));
				uint8* MyRow = Ours->FindRowUnchecked(Name);
				uint8* TheirRow = Theirs->FindRowUnchecked(Name);
				const FProperty* MyProperty = nullptr;
				const FProperty* TheirProperty = nullptr;
				void* MyValue = nullptr;
				void* TheirValue = nullptr;
				if (!MyRow || !TheirRow || !FindValue(RowStruct, MyRow, Field, MyProperty, MyValue) || !FindValue(RowStruct, TheirRow, Field, TheirProperty, TheirValue)
					|| !CopyValue(MyProperty, MyValue, Ours, TheirProperty, TheirValue, Retarget))
					OutProblems.Add(FString::Printf(TEXT("Could not set '%s' of row '%s'."), *Field, *Name.ToString()));
			}

			Ours->HandleDataTableChanged();
			return OutProblems.Num() == 0;
		}

		bool ApplyMaterialInstance(const TArray<const FDecision*>& Taken, UMaterialInstanceConstant* Ours, UMaterialInstanceConstant* Theirs, const FRetarget& Retarget, TArray<FString>& OutProblems)
		{
			// The parent first: parameters are checked against it.
			for (const FDecision* Decision : Taken)
			{
				if (Decision->Key == TEXT("prop/Parent"))
				{
					UMaterialInterface* Parent = Theirs->Parent;
					if (Parent && Parent->GetPackage() == Theirs->GetPackage())
						OutProblems.Add(TEXT("Theirs' parent material is inside the asset itself."));
					else
						Ours->SetParentEditorOnly(Parent, /*RecacheShader=*/false);
				}
			}

			FStaticParameterSet Static = Ours->GetStaticParameters();
			const FStaticParameterSet TheirStatic = Theirs->GetStaticParameters();

			for (const FDecision* Decision : Taken)
			{
				const FString& Key = Decision->Key;
				if (Key == TEXT("prop/Parent"))
					continue;

				if (Key.StartsWith(TEXT("prop/")))
				{
					ApplyProperty(*Decision, Ours, Theirs, Retarget, OutProblems);
				}
				else if (Key.StartsWith(TEXT("param/")))
				{
					FString ArrayName, Id;
					Key.Mid(6).Split(TEXT("/"), &ArrayName, &Id);
					const FArrayProperty* Array = FindFProperty<FArrayProperty>(Ours->GetClass(), FName(*ArrayName));
					const FStructProperty* InfoProperty = Array ? ParameterInfoOf(Array) : nullptr;
					if (!InfoProperty)
					{
						OutProblems.Add(FString::Printf(TEXT("Could not find the parameter list '%s'."), *ArrayName));
						continue;
					}

					void* MyMemory = Array->ContainerPtrToValuePtr<void>(Ours);
					void* TheirMemory = Array->ContainerPtrToValuePtr<void>(Theirs);
					FScriptArrayHelper Mine(Array, MyMemory);
					FScriptArrayHelper Their(Array, TheirMemory);
					int32 MyIndex = FindParameter(Array, InfoProperty, MyMemory, Id);
					const int32 TheirIndex = FindParameter(Array, InfoProperty, TheirMemory, Id);

					if (TheirIndex == INDEX_NONE)
					{
						if (MyIndex != INDEX_NONE)
							Mine.RemoveValues(MyIndex, 1);
						continue;
					}

					if (MyIndex == INDEX_NONE)
						MyIndex = Mine.AddValue();
					if (!CopyValue(Array->Inner, Mine.GetRawPtr(MyIndex), Ours, Array->Inner, Their.GetRawPtr(TheirIndex), Retarget))
						OutProblems.Add(FString::Printf(TEXT("Could not set parameter '%s'."), *Id));
				}
				else if (Key.StartsWith(TEXT("switch/")))
				{
					const FString Id = Key.Mid(7);
					auto Matches = [&Id](const FStaticSwitchParameter& Switch) { return ParameterId(Switch.ParameterInfo) == Id; };
					const FStaticSwitchParameter* TheirSwitch = TheirStatic.StaticSwitchParameters.FindByPredicate(Matches);
					Static.StaticSwitchParameters.RemoveAll(Matches);
					if (TheirSwitch && TheirSwitch->bOverride)
						Static.StaticSwitchParameters.Add(*TheirSwitch);
				}
				else
				{
					OutProblems.Add(FString::Printf(TEXT("Not supported yet: %s"), *Key));
				}
			}

			if (OutProblems.Num() > 0)
				return false;

			Ours->UpdateStaticPermutation(Static);
			Ours->PostEditChange();
			return true;
		}
	}

	FSnapshot TakeDataSnapshot(const UObject* Asset)
	{
		FSnapshot Snap;
		const FNormalizer Normalize(Asset);

		switch (GetAssetKind(Asset))
		{
		case EAssetKind::DataAsset:
			Snap.Facts.Add(TEXT("class"), GetPathNameSafe(Asset->GetClass()));
			Snap.Labels.Add(TEXT("class"), TEXT("Asset class"));
			Snap.Unmergeable.Add(TEXT("class"));
			AddObjectProperties(Snap, Asset, Normalize, [](const FProperty*) { return false; });
			break;
		case EAssetKind::DataTable:
			SnapshotDataTable(Snap, CastChecked<UDataTable>(Asset), Normalize);
			break;
		case EAssetKind::MaterialInstance:
			SnapshotMaterialInstance(Snap, CastChecked<UMaterialInstanceConstant>(Asset), Normalize);
			break;
		default:
			return Snap;
		}

		AddSubobjects(Snap, Asset, Normalize);
		return Snap;
	}

	FSnapshot TakeAssetSnapshot(const UObject* Asset)
	{
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
			return TakeSnapshot(Blueprint);
		return TakeDataSnapshot(Asset);
	}

	bool ApplyTheirsToData(const FMergePlan& Plan, UObject* Ours, UObject* Theirs, TArray<FString>& OutProblems)
	{
		const FRetarget Retarget(Theirs, Ours);

		TArray<const FDecision*> Taken;
		for (const FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Resolution != EResolution::Theirs || Decision.bPartOfWhole)
				continue;

			if (Decision.Key == TEXT("class") || Decision.Key == TEXT("rowstruct") || Decision.Key == TEXT("staticother") || Decision.Key.StartsWith(TEXT("subobject/")))
			{
				OutProblems.Add(FString::Printf(TEXT("Not supported yet: %s"), *Decision.Key));
				continue;
			}
			Taken.Add(&Decision);
		}

		if (OutProblems.Num() > 0)
			return false;

		switch (GetAssetKind(Ours))
		{
		case EAssetKind::DataAsset:
			for (const FDecision* Decision : Taken)
			{
				if (Decision->Key.StartsWith(TEXT("prop/")))
					ApplyProperty(*Decision, Ours, Theirs, Retarget, OutProblems);
				else
					OutProblems.Add(FString::Printf(TEXT("Not supported yet: %s"), *Decision->Key));
			}
			if (OutProblems.Num() > 0)
				return false;
			Ours->PostEditChange();
			return true;

		case EAssetKind::DataTable:
			return ApplyDataTable(Taken, CastChecked<UDataTable>(Ours), CastChecked<UDataTable>(Theirs), Retarget, OutProblems);

		case EAssetKind::MaterialInstance:
			return ApplyMaterialInstance(Taken, CastChecked<UMaterialInstanceConstant>(Ours), CastChecked<UMaterialInstanceConstant>(Theirs), Retarget, OutProblems);

		default:
			OutProblems.Add(TEXT("This asset type can't be merged."));
			return false;
		}
	}
}
