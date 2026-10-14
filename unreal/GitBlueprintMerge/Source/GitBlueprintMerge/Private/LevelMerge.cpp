// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#include "LevelMerge.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterActorList.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogGitLevelMerge, Log, All);

namespace GitBlueprintMerge
{
	namespace
	{
		const TCHAR* SideNames[] = {TEXT("Base"), TEXT("Ours"), TEXT("Theirs")};

		/** Stands for "the property isn't there" when values are compared. */
		const FString Missing(TEXT("\x01missing"));

		/** "comp/LightComponent0/LightColor" -> Name "Light Color", Component "LightComponent0". */
		void SplitKey(const FString& Key, FString& OutName, FString& OutComponent)
		{
			TArray<FString> Parts;
			Key.ParseIntoArray(Parts, TEXT("/"));
			OutName = Parts.Num() > 0 ? FName::NameToDisplayString(Parts.Last(), false) : Key;
			OutComponent = Parts.Num() == 3 ? Parts[1] : FString();
		}

		/** A change between revisions; Facts are base, ours, theirs. */
		FLevelChange MakeChange(const FString& Key, const TMap<FString, FString> (&Facts)[3], const TMap<FString, FString>& Categories, bool bHasBase)
		{
			const FString* Values[] = {Facts[0].Find(Key), Facts[1].Find(Key), Facts[2].Find(Key)};

			FLevelChange Change;
			Change.Key = Key;
			SplitKey(Key, Change.Name, Change.Component);
			const FString* Category = Categories.Find(Key);
			Change.Category = Category && !Category->IsEmpty() ? *Category : FString(TEXT("Other"));
			Change.Base = Values[0] ? *Values[0] : FString();
			Change.Ours = Values[1] ? *Values[1] : FString();
			Change.Theirs = Values[2] ? *Values[2] : FString();
			Change.bInBase = Values[0] != nullptr;
			Change.bInOurs = Values[1] != nullptr;
			Change.bInTheirs = Values[2] != nullptr;

			const FString BaseValue = Values[0] ? *Values[0] : Missing;
			const FString OursValue = Values[1] ? *Values[1] : Missing;
			const FString TheirsValue = Values[2] ? *Values[2] : Missing;
			const bool bOursChanged = !bHasBase || OursValue != BaseValue;
			const bool bTheirsChanged = !bHasBase || TheirsValue != BaseValue;
			if (bOursChanged && bTheirsChanged)
				Change.Kind = OursValue == TheirsValue ? ELevelChangeKind::Same : ELevelChangeKind::Both;
			else
				Change.Kind = bOursChanged ? ELevelChangeKind::OursOnly : ELevelChangeKind::TheirsOnly;
			return Change;
		}

		void SortChanges(TArray<FLevelChange>& Changes)
		{
			Changes.Sort([](const FLevelChange& A, const FLevelChange& B)
			{
				if (A.Category != B.Category)
					return A.Category < B.Category;
				if (A.Component != B.Component)
					return A.Component < B.Component;
				return A.Name < B.Name;
			});
		}

		AActor* FindActor(UPackage* Package)
		{
			AActor* Found = nullptr;
			if (Package)
			{
				ForEachObjectWithPackage(Package, [&Found](UObject* Object)
				{
					Found = Cast<AActor>(Object);
					return Found == nullptr;
				}, EGetObjectsFlags::None);
			}
			return Found;
		}

		/** Editable properties of an actor and its components as full text (not a delta), so every value can be shown. */
		TMap<FString, FString> TakeFacts(AActor* Actor, const FString& TempLevel, const FString& Level, const FString& Self, TMap<FString, FString>& OutCategories)
		{
			TMap<FString, FString> Facts;
			auto Add = [&](const FString& Prefix, UObject* Object)
			{
				for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
				{
					if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Transient | CPF_EditConst | CPF_DisableEditOnInstance))
						continue;

					FString Value;
					It->ExportTextItem_InContainer(Value, Object, nullptr, Object, PPF_None);
					if (!TempLevel.IsEmpty())
						Value.ReplaceInline(*TempLevel, *Level);
					Value.ReplaceInline(*Self, TEXT("<self>"));
					Facts.Add(Prefix + It->GetName(), Value);

					// The top Details category: "Light|Advanced" -> "Light".
					FString Category = It->GetMetaData(TEXT("Category"));
					Category.Split(TEXT("|"), &Category, nullptr);
					OutCategories.Add(Prefix + It->GetName(), Category);
				}
			};

			Add(TEXT("actor/"), Actor);
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (Component)
					Add(FString::Printf(TEXT("comp/%s/"), *Component->GetName()), Component);
			}
			return Facts;
		}
	}

	int32 FLevelActor::NumDecided() const
	{
		if (bExistenceConflict)
			return Existence != ELevelChoice::None ? 1 : 0;

		int32 Count = 0;
		for (const FLevelChange& Change : Changes)
		{
			if (Change.Choice != ELevelChoice::None)
				++Count;
		}
		return Count;
	}

	int32 FLevelActor::NumBothChanged() const
	{
		int32 Count = 0;
		for (const FLevelChange& Change : Changes)
		{
			if (Change.Kind == ELevelChangeKind::Both)
				++Count;
		}
		return Count;
	}

	FString FLevelActor::Result() const
	{
		if (bExistenceConflict)
		{
			const bool bKeep = (Existence == ELevelChoice::Ours && bInOurs) || (Existence == ELevelChoice::Theirs && bInTheirs);
			return bKeep ? TEXT("unchanged") : TEXT("deleted");
		}
		return TEXT("saved");
	}

	FLevelMergeSession::~FLevelMergeSession()
	{
		for (UPackage* Package : TempPackages)
		{
			if (Package)
				ResetLoaders(Package);
		}
		for (const FString& File : TempFiles)
			IFileManager::Get().Delete(*File, false, false, true);
	}

	FString FLevelMergeSession::ReadManifestLevel(const FString& ManifestFile)
	{
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (FFileHelper::LoadFileToString(Text, *ManifestFile) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root.IsValid())
			return Root->GetStringField(TEXT("level"));
		return FString();
	}

	bool FLevelMergeSession::Load(const FString& ManifestFile)
	{
		FString Text;
		TSharedPtr<FJsonObject> Root;
		if (!FFileHelper::LoadFileToString(Text, *ManifestFile) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			UE_LOG(LogGitLevelMerge, Error, TEXT("Could not read the manifest '%s'."), *ManifestFile);
			return false;
		}

		Level = Root->GetStringField(TEXT("level"));
		OursLabel = Root->GetStringField(TEXT("oursLabel"));
		TheirsLabel = Root->GetStringField(TEXT("theirsLabel"));

		// The editor shows the real level; ours is in the working copy.
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World || World->GetPackage()->GetName() != Level)
			World = UEditorLoadingAndSavingUtils::LoadMap(Level);
		if (!World)
		{
			UE_LOG(LogGitLevelMerge, Error, TEXT("Could not open the level %s."), *Level);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("actors")))
		{
			FLevelActor& Actor = Actors.AddDefaulted_GetRef();
			if (!LoadActor(Actor, Value->AsObject()))
				return false;
		}

		LoadLiveActors();
		for (FLevelActor& Actor : Actors)
		{
			if (!Actor.Live.IsValid())
				Problems.Add(FString::Printf(TEXT("%s is not in the open level."), *Actor.Label));
		}

		UE_LOG(LogGitLevelMerge, Display, TEXT("Level merge of %s: %d actor(s), %d item(s) to decide."), *Level, Actors.Num(), NumItems());
		return true;
	}

	bool FLevelMergeSession::LoadActor(FLevelActor& Actor, const TSharedPtr<FJsonObject>& Json)
	{
		Actor.Path = Json->GetStringField(TEXT("path"));
		Actor.PackageName = Json->GetStringField(TEXT("package"));
		Actor.Label = Json->GetStringField(TEXT("label"));
		Actor.ClassName = Json->GetStringField(TEXT("className"));

		TMap<FString, FString> Facts[3];
		TMap<FString, FString> Categories;
		bool* Present[] = {&Actor.bInBase, &Actor.bInOurs, &Actor.bInTheirs};
		const TCHAR* Fields[] = {TEXT("base"), TEXT("ours"), TEXT("theirs")};
		for (int32 Side = 0; Side < 3; ++Side)
		{
			const FString File = Json->GetStringField(Fields[Side]);
			*Present[Side] = !File.IsEmpty();
			if (File.IsEmpty())
				continue;

			if (!LoadRevision(File, SideNames[Side], Facts[Side], Categories))
			{
				UE_LOG(LogGitLevelMerge, Error, TEXT("Could not load the %s version of %s."), SideNames[Side], *Actor.Label);
				return false;
			}
		}

		TSet<FString> Keys;
		for (const TMap<FString, FString>& Map : Facts)
		{
			for (const TPair<FString, FString>& Pair : Map)
				Keys.Add(Pair.Key);
		}

		// One side deleted the actor: the only decision is keep or delete. What the other side changed is shown.
		if (!Actor.bInOurs || !Actor.bInTheirs)
		{
			Actor.bExistenceConflict = true;
			const int32 Kept = Actor.bInOurs ? 1 : 2;
			if (Actor.bInBase && (Actor.bInOurs || Actor.bInTheirs))
			{
				for (const FString& Key : Keys)
				{
					const FString* Base = Facts[0].Find(Key);
					const FString* Now = Facts[Kept].Find(Key);
					if ((Base ? *Base : Missing) != (Now ? *Now : Missing))
						Actor.KeptSideChanges.Add(MakeChange(Key, Facts, Categories, true));
				}
				SortChanges(Actor.KeptSideChanges);
			}
			return true;
		}

		for (const FString& Key : Keys)
		{
			const FString* Base = Facts[0].Find(Key);
			const FString* Ours = Facts[1].Find(Key);
			const FString* Theirs = Facts[2].Find(Key);
			const FString BaseValue = Base ? *Base : Missing;
			const FString OursValue = Ours ? *Ours : Missing;
			const FString TheirsValue = Theirs ? *Theirs : Missing;
			const bool bBaseDiffers = Actor.bInBase && (BaseValue != OursValue || BaseValue != TheirsValue);
			if (!bBaseDiffers && OursValue == TheirsValue)
				continue;

			Actor.Changes.Add(MakeChange(Key, Facts, Categories, Actor.bInBase));
		}

		SortChanges(Actor.Changes);
		return true;
	}

	/**
	 * Loads one revision of an actor package next to the others. Its outer (the level's PersistentLevel) lives in
	 * the level package, so an instancing context maps that package to a transient world of the same name.
	 */
	UPackage* FLevelMergeSession::LoadRevision(const FString& File, const TCHAR* Label, TMap<FString, FString>& OutFacts, TMap<FString, FString>& OutCategories)
	{
		const FString LevelName = FPackageName::GetShortName(Level);
		const FString Guid = FGuid::NewGuid().ToString();
		const FString TempLevel = FString::Printf(TEXT("/Temp/GBMLevel_%s_%s/%s"), Label, *Guid, *LevelName);

		UPackage* WorldPackage = CreatePackage(*TempLevel);
		WorldPackage->SetFlags(RF_Transient);
		UWorld::CreateWorld(EWorldType::Inactive, false, FName(*LevelName), WorldPackage, false, ERHIFeatureLevel::Num, nullptr, true);

		const FString TempFile = FPaths::Combine(FPaths::DiffDir(), FString::Printf(TEXT("GBMActor_%s_%s"), Label, *Guid)) + FPackageName::GetAssetPackageExtension();
		if (IFileManager::Get().Copy(*TempFile, *File) != COPY_OK)
			return nullptr;
		TempFiles.Add(TempFile);

		FLinkerInstancingContext Context;
		Context.AddPackageMapping(FName(*Level), FName(*TempLevel));
		UPackage* Package = LoadPackage(nullptr, FPackagePath::FromLocalPath(TempFile), LOAD_None, nullptr, &Context);
		AActor* Actor = FindActor(Package);
		if (!Actor)
			return nullptr;

		TempPackages.Add(Package);
		OutFacts = TakeFacts(Actor, TempLevel, Level, Package->GetName(), OutCategories);
		return Package;
	}

	/** World Partition keeps spatially loaded actors unloaded until something asks for them. */
	void FLevelMergeSession::LoadLiveActors()
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr)
		{
			TArray<FGuid> ToLoad;
			FWorldPartitionHelpers::ForEachActorDescInstance(WorldPartition, AActor::StaticClass(), [this, &ToLoad](const FWorldPartitionActorDescInstance* Desc)
			{
				const FString Package = Desc->GetActorPackage().ToString();
				if (Actors.ContainsByPredicate([&Package](const FLevelActor& Actor) { return Actor.PackageName == Package; }))
					ToLoad.Add(Desc->GetGuid());
				return true;
			});

			if (ToLoad.Num() > 0)
			{
				Loader = MakeUnique<FLoaderAdapterActorList>(World);
				Loader->AddActors(ToLoad);
				Loader->Load();
			}
		}

		for (FLevelActor& Actor : Actors)
			Actor.Live = FindActor(FindPackage(nullptr, *Actor.PackageName));
	}

	int32 FLevelMergeSession::NumItems() const
	{
		int32 Count = 0;
		for (const FLevelActor& Actor : Actors)
			Count += Actor.NumItems();
		return Count;
	}

	int32 FLevelMergeSession::NumDecided() const
	{
		int32 Count = 0;
		for (const FLevelActor& Actor : Actors)
			Count += Actor.NumDecided();
		return Count;
	}

	void FLevelMergeSession::Choose(int32 ActorIndex, int32 ChangeIndex, ELevelChoice Choice)
	{
		if (!Actors.IsValidIndex(ActorIndex) || !Actors[ActorIndex].Changes.IsValidIndex(ChangeIndex))
			return;

		FLevelActor& Actor = Actors[ActorIndex];
		FLevelChange& Change = Actor.Changes[ChangeIndex];
		Change.Choice = Choice;
		if (Choice != ELevelChoice::Ours && Choice != ELevelChoice::Theirs)
			return;

		const bool bPresent = Choice == ELevelChoice::Ours ? Change.bInOurs : Change.bInTheirs;
		if (bPresent)
			Apply(Actor, Change, Choice == ELevelChoice::Ours ? Change.Ours : Change.Theirs);
		Change.Applied = GetLiveValue(ActorIndex, ChangeIndex).Get(FString());
	}

	void FLevelMergeSession::ChooseAll(int32 ActorIndex, const FString& Category, ELevelChoice Choice)
	{
		for (int32 Index = 0; Index < Actors.Num(); ++Index)
		{
			if (ActorIndex != INDEX_NONE && Index != ActorIndex)
				continue;

			FLevelActor& Actor = Actors[Index];
			if (Actor.bExistenceConflict)
			{
				if (Category.IsEmpty() && (Choice == ELevelChoice::Ours || Choice == ELevelChoice::Theirs))
					ChooseExistence(Index, Choice);
				continue;
			}

			for (int32 ChangeIndex = 0; ChangeIndex < Actor.Changes.Num(); ++ChangeIndex)
			{
				if (Category.IsEmpty() || Actor.Changes[ChangeIndex].Category == Category)
					Choose(Index, ChangeIndex, Choice);
			}
		}
	}

	bool FLevelMergeSession::IsAllChosen(int32 ActorIndex, const FString& Category, ELevelChoice Choice) const
	{
		int32 Count = 0;
		for (int32 Index = 0; Index < Actors.Num(); ++Index)
		{
			if (ActorIndex != INDEX_NONE && Index != ActorIndex)
				continue;

			const FLevelActor& Actor = Actors[Index];
			if (Actor.bExistenceConflict)
			{
				if (Category.IsEmpty() && Choice != ELevelChoice::Level)
				{
					if (Actor.Existence != Choice)
						return false;
					++Count;
				}
				continue;
			}

			for (const FLevelChange& Change : Actor.Changes)
			{
				if (!Category.IsEmpty() && Change.Category != Category)
					continue;
				if (Change.Choice != Choice)
					return false;
				++Count;
			}
		}
		return Count > 0;
	}

	bool FLevelMergeSession::FindLiveProperty(const FLevelActor& Actor, const FLevelChange& Change, UObject*& OutTarget, FProperty*& OutProperty) const
	{
		OutTarget = nullptr;
		OutProperty = nullptr;
		AActor* Live = Actor.Live.Get();
		if (!Live)
			return false;

		TArray<FString> Parts;
		Change.Key.ParseIntoArray(Parts, TEXT("/"));
		if (Parts.Num() == 2 && Parts[0] == TEXT("actor"))
		{
			OutTarget = Live;
		}
		else if (Parts.Num() == 3 && Parts[0] == TEXT("comp"))
		{
			for (UActorComponent* Component : Live->GetComponents())
			{
				if (Component && Component->GetName() == Parts[1])
					OutTarget = Component;
			}
		}

		OutProperty = OutTarget ? FindFProperty<FProperty>(OutTarget->GetClass(), *Parts.Last()) : nullptr;
		return OutProperty != nullptr;
	}

	TOptional<FString> FLevelMergeSession::GetLiveValue(int32 ActorIndex, int32 ChangeIndex) const
	{
		if (!Actors.IsValidIndex(ActorIndex) || !Actors[ActorIndex].Changes.IsValidIndex(ChangeIndex))
			return {};

		const FLevelActor& Actor = Actors[ActorIndex];
		UObject* Target;
		FProperty* Property;
		if (!FindLiveProperty(Actor, Actor.Changes[ChangeIndex], Target, Property))
			return {};

		FString Value;
		Property->ExportTextItem_InContainer(Value, Target, nullptr, Target, PPF_None);
		Value.ReplaceInline(*Actor.Live->GetPackage()->GetName(), TEXT("<self>"));
		return Value;
	}

	bool FLevelMergeSession::IsEditedInLevel(int32 ActorIndex, int32 ChangeIndex) const
	{
		if (!Actors.IsValidIndex(ActorIndex) || !Actors[ActorIndex].Changes.IsValidIndex(ChangeIndex))
			return false;

		const FLevelChange& Change = Actors[ActorIndex].Changes[ChangeIndex];
		if (Change.Choice != ELevelChoice::Ours && Change.Choice != ELevelChoice::Theirs)
			return false;

		const TOptional<FString> Live = GetLiveValue(ActorIndex, ChangeIndex);
		return Live.IsSet() && Live.GetValue() != Change.Applied;
	}

	int32 FLevelMergeSession::NumEditedInLevel() const
	{
		int32 Count = 0;
		for (int32 ActorIndex = 0; ActorIndex < Actors.Num(); ++ActorIndex)
		{
			for (int32 ChangeIndex = 0; ChangeIndex < Actors[ActorIndex].Changes.Num(); ++ChangeIndex)
			{
				if (IsEditedInLevel(ActorIndex, ChangeIndex))
					++Count;
			}
		}
		return Count;
	}

	int32 FLevelMergeSession::NumChosen(ELevelChoice Choice) const
	{
		int32 Count = 0;
		for (const FLevelActor& Actor : Actors)
		{
			for (const FLevelChange& Change : Actor.Changes)
			{
				if (Change.Choice == Choice)
					++Count;
			}
		}
		return Count;
	}

	int32 FLevelMergeSession::NumResults(const TCHAR* Result) const
	{
		int32 Count = 0;
		for (const FLevelActor& Actor : Actors)
		{
			if (Actor.Result() == Result)
				++Count;
		}
		return Count;
	}

	int32 FLevelMergeSession::NumOtherDirtyPackages() const
	{
		TArray<UPackage*> Dirty;
		FEditorFileUtils::GetDirtyWorldPackages(Dirty);
		FEditorFileUtils::GetDirtyContentPackages(Dirty);

		int32 Count = 0;
		for (UPackage* Package : Dirty)
		{
			const FString Name = Package->GetName();
			if (!Actors.ContainsByPredicate([&Name](const FLevelActor& Actor) { return Actor.PackageName == Name; }))
				++Count;
		}
		return Count;
	}

	void FLevelMergeSession::ChooseExistence(int32 ActorIndex, ELevelChoice Choice)
	{
		if (!Actors.IsValidIndex(ActorIndex))
			return;

		FLevelActor& Actor = Actors[ActorIndex];
		Actor.Existence = Choice;

		// A deleted actor is only hidden here; GitO removes its file.
		if (AActor* Live = Actor.Live.Get())
		{
			Live->SetIsTemporarilyHiddenInEditor(Actor.Result() == TEXT("deleted"));
			if (GEditor)
				GEditor->RedrawLevelEditingViewports();
		}
	}

	void FLevelMergeSession::Apply(FLevelActor& Actor, FLevelChange& Change, const FString& Value)
	{
		AActor* Live = Actor.Live.Get();
		UObject* Target;
		FProperty* Property;
		if (!FindLiveProperty(Actor, Change, Target, Property))
		{
			UE_LOG(LogGitLevelMerge, Warning, TEXT("%s: can't set %s (not on the actor in the level)."), *Actor.Label, *Change.Name);
			return;
		}

		FString Text = Value.Replace(TEXT("<self>"), *Live->GetPackage()->GetName());

		// An empty container exports as nothing, but only "()" imports as empty.
		if (Text.IsEmpty() && (Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>() || Property->IsA<FMapProperty>()))
			Text = TEXT("()");

		Live->Modify();
		Target->Modify();
		Target->PreEditChange(Property);
		Property->ImportText_InContainer(*Text, Target, Target, PPF_None);
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		Target->PostEditChangeProperty(Event);
		if (Target != Live && Cast<USceneComponent>(Target))
			Live->PostEditMove(true);

		if (GEditor)
			GEditor->RedrawLevelEditingViewports();
	}

	void FLevelMergeSession::Focus(int32 ActorIndex) const
	{
		if (!GEditor || !Actors.IsValidIndex(ActorIndex))
			return;

		if (AActor* Live = Actors[ActorIndex].Live.Get())
		{
			GEditor->SelectNone(false, true);
			GEditor->SelectActor(Live, true, true, true);
			GEditor->MoveViewportCamerasToActor(*Live, false);
		}
	}

	bool FLevelMergeSession::Save()
	{
		TArray<UPackage*> Packages;
		for (FLevelActor& Actor : Actors)
		{
			AActor* Live = Actor.Live.Get();
			if (Actor.Result() != TEXT("saved") || !Live)
				continue;

			UPackage* Package = Live->GetPackage();
			const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*File, false);
			Package->MarkPackageDirty();
			Packages.Add(Package);
		}

		if (Packages.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(Packages, false))
		{
			Problems.Add(TEXT("Unreal couldn't save the merged actors."));
			return false;
		}

		Discard();
		bWritten = true;
		return true;
	}

	void FLevelMergeSession::Discard()
	{
		for (FLevelActor& Actor : Actors)
		{
			if (AActor* Live = Actor.Live.Get())
				Live->GetPackage()->SetDirtyFlag(false);
		}
	}

	void FLevelMergeSession::WriteReport(const FString& File) const
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetBoolField(TEXT("written"), bWritten);

		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FLevelActor& Actor : Actors)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("path"), Actor.Path);
			Item->SetStringField(TEXT("label"), Actor.Label);
			Item->SetStringField(TEXT("result"), Actor.Result());
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		Root->SetArrayField(TEXT("actors"), Items);

		TArray<TSharedPtr<FJsonValue>> ProblemValues;
		for (const FString& Problem : Problems)
			ProblemValues.Add(MakeShared<FJsonValueString>(Problem));
		Root->SetArrayField(TEXT("problems"), ProblemValues);

		FString Text;
		FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
		FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}
