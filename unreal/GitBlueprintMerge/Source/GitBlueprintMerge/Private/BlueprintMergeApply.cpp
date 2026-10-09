// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#include "BlueprintMergeApply.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionTerminator.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace GitBlueprintMerge
{
	namespace
	{
		/** Rewrites references to Theirs' temp package so they point at Ours. */
		struct FRetarget
		{
			FString From;
			FString To;
			FString FromShort;
			FString ToShort;

			FRetarget(const UBlueprint* Theirs, const UBlueprint* Ours)
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

			UObject* operator()(UObject* Object) const
			{
				if (!Object)
					return nullptr;

				const FString Path = Object->GetPathName();
				const FString NewPath = (*this)(Path);
				return NewPath == Path ? Object : FindObject<UObject>(nullptr, *NewPath);
			}
		};

		TMap<FString, UEdGraphNode*> NodesById(const UBlueprint* Blueprint)
		{
			TMap<FString, UEdGraphNode*> Nodes;
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (const UEdGraph* Graph : Graphs)
			{
				if (!Graph)
					continue;

				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node)
						Nodes.Add(Node->NodeGuid.ToString(), Node);
				}
			}
			return Nodes;
		}

		TMap<FString, UEdGraph*> GraphsById(const UBlueprint* Blueprint)
		{
			TMap<FString, UEdGraph*> Graphs;
			TArray<UEdGraph*> All;
			Blueprint->GetAllGraphs(All);
			for (UEdGraph* Graph : All)
			{
				if (Graph)
					Graphs.Add(GraphId(Graph), Graph);
			}
			return Graphs;
		}

		UEdGraphPin* FindPin(const UEdGraphNode* Node, const FString& Key)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && PinKey(Pin) == Key)
					return Pin;
			}
			return nullptr;
		}

		FBPVariableDescription* FindVariable(UBlueprint* Blueprint, const FString& Id)
		{
			for (FBPVariableDescription& Var : Blueprint->NewVariables)
			{
				if (Var.VarGuid.ToString() == Id)
					return &Var;
			}
			return nullptr;
		}

		/** "<prefix><id>/<detail>" -> id, detail (empty for the entity itself). */
		void SplitEntityKey(const FString& Key, int32 PrefixLength, FString& Id, FString& Detail)
		{
			if (!Key.Mid(PrefixLength).Split(TEXT("/"), &Id, &Detail))
			{
				Id = Key.Mid(PrefixLength);
				Detail.Reset();
			}
		}

		FString NodeOfKey(const FString& Key)
		{
			FString Id, Detail, Pin;
			if (Key.StartsWith(TEXT("node/")))
				SplitEntityKey(Key, 5, Id, Detail);
			else if (Key.StartsWith(TEXT("pin/")))
				SplitPinRef(Key.Mid(4), Id, Pin);
			return Id;
		}

		USCS_Node* FindComponent(const UBlueprint* Blueprint, const FString& Id)
		{
			if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
			{
				for (USCS_Node* Node : SCS->GetAllNodes())
				{
					if (Node && Node->VariableGuid.ToString() == Id)
						return Node;
				}
			}
			return nullptr;
		}

		/** Function entry/result nodes can't be copied like other nodes; give ours theirs' pins and settings instead. */
		void SyncTerminator(UK2Node_FunctionTerminator* Mine, const UK2Node_FunctionTerminator* Their)
		{
			Mine->Modify();
			Mine->UserDefinedPins.Reset();
			for (const TSharedPtr<FUserPinInfo>& Pin : Their->UserDefinedPins)
			{
				if (Pin.IsValid())
					Mine->UserDefinedPins.Add(MakeShared<FUserPinInfo>(*Pin));
			}

			UK2Node_FunctionEntry* MyEntry = Cast<UK2Node_FunctionEntry>(Mine);
			const UK2Node_FunctionEntry* TheirEntry = Cast<UK2Node_FunctionEntry>(Their);
			if (MyEntry && TheirEntry)
			{
				MyEntry->SetExtraFlags(TheirEntry->GetExtraFlags());
				MyEntry->MetaData = TheirEntry->MetaData;
				MyEntry->LocalVariables = TheirEntry->LocalVariables;
			}

			Mine->NodePosX = Their->NodePosX;
			Mine->NodePosY = Their->NodePosY;
			Mine->ReconstructNode();
		}

		/** Objects a Blueprint owns, by path below it ("WidgetTree.Title"), as the snapshot's "inner/" keys use them. */
		TMap<FString, UObject*> InnerObjects(const UBlueprint* Blueprint)
		{
			TMap<FString, UObject*> Objects;
			TArray<UObject*> Inner;
			GetObjectsWithOuter(Blueprint, Inner, EGetObjectsFlags::IncludeNestedObjects);
			for (UObject* Object : Inner)
				Objects.Add(Object->GetPathName(Blueprint), Object);
			return Objects;
		}

		/** Copies every setting of Their onto Mine (same class) except Skip, for a widget or slot theirs added. */
		bool CopyAllSettings(UObject* Mine, const UObject* Their, std::initializer_list<FName> Skip, const FRetarget& Retarget)
		{
			bool bOk = true;
			for (TFieldIterator<FProperty> It(Their->GetClass()); It; ++It)
			{
				const FProperty* Property = *It;
				if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || Property->ArrayDim != 1 ||
					Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance) ||
					Property->IsA<FDelegateProperty>() || Property->IsA<FMulticastDelegateProperty>())
					continue;

				bool bSkipped = false;
				for (const FName& Name : Skip)
					bSkipped |= Property->GetFName() == Name;
				if (bSkipped)
					continue;

				// Straight into memory: setters (e.g. SetCursor) would also flip their override flags.
				FString Text;
				Property->ExportTextItem_Direct(Text, Property->ContainerPtrToValuePtr<void>(Their), nullptr, nullptr, PPF_None);
				bOk &= Property->ImportText_Direct(*Retarget(Text), Property->ContainerPtrToValuePtr<void>(Mine), Mine, PPF_None) != nullptr;
			}
			return bOk;
		}

		/** Gives a widget theirs added the variable guid theirs gave it (the designer keeps one per widget). */
		void CopyWidgetGuid(UBlueprint* Ours, const UBlueprint* Theirs, FName Name)
		{
			const FMapProperty* Property = FindFProperty<FMapProperty>(Ours->GetClass(), TEXT("WidgetVariableNameToGuidMap"));
			if (!Property || Property->KeyProp->GetClass() != FNameProperty::StaticClass())
				return;

			FScriptMapHelper TheirMap(Property, Property->ContainerPtrToValuePtr<void>(Theirs));
			const int32 Index = TheirMap.FindMapIndexWithKey(&Name);
			if (Index == INDEX_NONE)
				return;

			FScriptMapHelper MyMap(Property, Property->ContainerPtrToValuePtr<void>(Ours));
			MyMap.AddPair(&Name, TheirMap.GetValuePtr(Index));
		}

		/** Copies one value from Their to Mine through text; Path is "<property>" or "<struct property>.<field>". */
		bool CopySetting(UObject* Mine, const UObject* Their, const FString& Path, const FRetarget& Retarget)
		{
			FString Name = Path, Field;
			Path.Split(TEXT("."), &Name, &Field);

			const FProperty* MyProperty = FindFProperty<FProperty>(Mine->GetClass(), FName(*Name));
			const FProperty* TheirProperty = FindFProperty<FProperty>(Their->GetClass(), FName(*Name));
			if (!MyProperty || !TheirProperty || MyProperty->ArrayDim != 1)
				return false;

			void* MyValue = MyProperty->ContainerPtrToValuePtr<void>(Mine);
			const void* TheirValue = TheirProperty->ContainerPtrToValuePtr<void>(Their);
			if (!Field.IsEmpty())
			{
				const FStructProperty* MyStruct = CastField<FStructProperty>(MyProperty);
				const FStructProperty* TheirStruct = CastField<FStructProperty>(TheirProperty);
				MyProperty = MyStruct ? FindFProperty<FProperty>(MyStruct->Struct, FName(*Field)) : nullptr;
				TheirProperty = TheirStruct ? FindFProperty<FProperty>(TheirStruct->Struct, FName(*Field)) : nullptr;
				if (!MyProperty || !TheirProperty || MyProperty->ArrayDim != 1)
					return false;
				MyValue = MyProperty->ContainerPtrToValuePtr<void>(MyValue);
				TheirValue = TheirProperty->ContainerPtrToValuePtr<void>(TheirValue);
			}

			FString Text;
			TheirProperty->ExportTextItem_Direct(Text, TheirValue, nullptr, nullptr, PPF_None);
			Mine->Modify();
			return MyProperty->ImportText_Direct(*Retarget(Text), MyValue, Mine, PPF_None) != nullptr;
		}
	}

	bool ApplyTheirs(const FMergePlan& Plan, UBlueprint* Ours, UBlueprint* Theirs, TArray<FString>& OutProblems)
	{
		const FRetarget Retarget(Theirs, Ours);
		const TMap<FString, UEdGraphNode*> TheirNodes = NodesById(Theirs);
		const TMap<FString, UEdGraphNode*> OurNodesBefore = NodesById(Ours);
		const TMap<FString, UEdGraph*> TheirGraphs = GraphsById(Theirs);

		// Nodes ours changed itself; replacing them with theirs' version would lose that.
		TSet<FString> NodesChangedByOurs;
		for (const FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Resolution == EResolution::Ours)
				NodesChangedByOurs.Add(NodeOfKey(Decision.Key));
		}

		// 1. Sort what we take from theirs into work lists.
		TArray<FString> NodesToAdd, NodesToRemove, NodesToReplace;
		TArray<const FDecision*> NodeEdits, PinEdits, VariableChanges, ClassDefaults, GraphChanges, ComponentEdits, ClassSettings, WidgetsToAdd, WidgetEdits;

		for (const FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Resolution != EResolution::Theirs || Decision.bPartOfWhole)
				continue;

			const FString& Key = Decision.Key;
			FString Src, Dst;
			if (SplitLink(Key, Src, Dst))
				continue; // links are reconciled at the end

			if (Key.StartsWith(TEXT("node/")))
			{
				FString Id, Detail;
				SplitEntityKey(Key, 5, Id, Detail);
				if (!Detail.IsEmpty() && Detail != TEXT("title"))
					NodeEdits.Add(&Decision);
				else if (Detail == TEXT("title"))
					NodesToReplace.AddUnique(Id);
				else if (!Decision.Theirs.IsSet())
					NodesToRemove.Add(Id);
				else if (!Decision.Base.IsSet() || !OurNodesBefore.Contains(Id))
					NodesToAdd.Add(Id); // added by theirs, or ours removed it and the conflict was decided for theirs
				else
					OutProblems.Add(FString::Printf(TEXT("Theirs moved node %s to another graph; not supported yet."), *Id));
			}
			else if (Key.StartsWith(TEXT("pin/")))
			{
				// A pin that appeared or disappeared means the node itself was reconfigured.
				if (Decision.Base.IsSet() && Decision.Theirs.IsSet())
					PinEdits.Add(&Decision);
				else
					NodesToReplace.AddUnique(NodeOfKey(Key));
			}
			else if (Key.StartsWith(TEXT("var/")))
			{
				VariableChanges.Add(&Decision);
			}
			else if (Key.StartsWith(TEXT("cdo/")))
			{
				if (Decision.Theirs.IsSet())
					ClassDefaults.Add(&Decision);
			}
			else if (Key.StartsWith(TEXT("graph/")))
			{
				GraphChanges.Add(&Decision);
			}
			else if (Key.StartsWith(TEXT("component/")) && Key.Contains(TEXT("/prop/")))
			{
				ComponentEdits.Add(&Decision);
			}
			else if (Key.StartsWith(TEXT("bp/")))
			{
				ClassSettings.Add(&Decision);
			}
			else if (Key.StartsWith(TEXT("inner/")))
			{
				(Key.Mid(6).Contains(TEXT("/")) ? WidgetEdits : WidgetsToAdd).Add(&Decision);
			}
			else
			{
				OutProblems.Add(FString::Printf(TEXT("Not supported yet: %s"), *Key));
			}
		}

		// Function entry/result nodes theirs reconfigured (new inputs, renamed function, ...) are updated in place.
		TArray<FString> TerminatorsToSync;
		for (int32 Index = NodesToReplace.Num() - 1; Index >= 0; --Index)
		{
			UEdGraphNode* const* Their = TheirNodes.Find(NodesToReplace[Index]);
			if (Their && (*Their)->IsA<UK2Node_FunctionTerminator>() && OurNodesBefore.Contains(NodesToReplace[Index]))
			{
				TerminatorsToSync.Add(NodesToReplace[Index]);
				NodesToReplace.RemoveAt(Index);
			}
		}

		for (const FString& Id : NodesToReplace)
		{
			if (NodesChangedByOurs.Contains(Id))
				OutProblems.Add(FString::Printf(TEXT("Node %s was reconfigured in theirs and edited in ours."), *Id));
		}

		if (OutProblems.Num() > 0)
			return false;

		// 2. Member variables. Removals first, so a variable theirs added can take a name ours gives up.
		VariableChanges.StableSort([](const FDecision& A, const FDecision& B) { return !A.Theirs.IsSet() && B.Theirs.IsSet(); });
		for (const FDecision* Decision : VariableChanges)
		{
			FString Id, Detail;
			SplitEntityKey(Decision->Key, 4, Id, Detail);
			FBPVariableDescription* Mine = FindVariable(Ours, Id);
			const FBPVariableDescription* Their = FindVariable(Theirs, Id);

			if (!Detail.IsEmpty())
			{
				if (Mine && Their)
					Mine->DefaultValue = Retarget(Their->DefaultValue);
			}
			else if (!Mine && Their)
			{
				FBPVariableDescription& Added = Ours->NewVariables.Add_GetRef(*Their);
				Added.DefaultValue = Retarget(Their->DefaultValue);
			}
			else if (!Decision->Theirs.IsSet() && Mine)
			{
				FBlueprintEditorUtils::RemoveMemberVariable(Ours, Mine->VarName);
			}
			else if (Mine && Their)
			{
				if (Mine->VarName != Their->VarName)
					FBlueprintEditorUtils::RenameMemberVariable(Ours, Mine->VarName, Their->VarName);

				Mine = FindVariable(Ours, Id);
				Mine->VarType = Their->VarType;
				Mine->Category = Their->Category;
				Mine->PropertyFlags = Their->PropertyFlags;
			}
			else
			{
				OutProblems.Add(FString::Printf(TEXT("Could not apply variable change %s."), *Decision->Key));
			}
		}

		if (VariableChanges.Num() > 0)
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ours);

		// 3. Nodes theirs removed or reconfigured go away; theirs' version is copied in below.
		{
			const TMap<FString, UEdGraphNode*> OurNodes = NodesById(Ours);
			for (const TArray<FString>* List : {&NodesToRemove, &NodesToReplace})
			{
				for (const FString& Id : *List)
				{
					if (UEdGraphNode* const* Node = OurNodes.Find(Id))
						FBlueprintEditorUtils::RemoveNode(Ours, *Node, /*bDontRecompile=*/true);
				}
			}
		}

		// 3b. Function graphs theirs added, removed or renamed. A new function starts with its entry node made to
		// match theirs; the rest of its nodes are copied in like any other added node.
		TSet<FString> NodesInPlace;
		for (const FDecision* Decision : GraphChanges)
		{
			const FString Id = Decision->Key.Mid(6);
			const TMap<FString, UEdGraph*> OurGraphs = GraphsById(Ours);
			UEdGraph* const* Mine = OurGraphs.Find(Id);
			UEdGraph* const* Their = TheirGraphs.Find(Id);

			if (!Decision->Theirs.IsSet())
			{
				if (Mine)
					FBlueprintEditorUtils::RemoveGraph(Ours, *Mine, EGraphRemoveFlags::MarkTransient);
				continue;
			}

			if (!Their || !(*Their)->GetSchema())
			{
				OutProblems.Add(FString::Printf(TEXT("Graph %s is missing from theirs."), *Id));
				continue;
			}

			const FString Name = (*Their)->GetName();
			if (Mine)
			{
				if ((*Mine)->GetName() != Name)
					FBlueprintEditorUtils::RenameGraph(*Mine, Name);
				continue;
			}

			if (FindObject<UObject>(Ours, *Name) || FBlueprintEditorUtils::FindNewVariableIndex(Ours, FName(*Name)) != INDEX_NONE)
			{
				OutProblems.Add(FString::Printf(TEXT("Theirs added the function '%s', but ours already has something with that name."), *Name));
				continue;
			}

			UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Ours, FName(*Name), (*Their)->GetClass(), (*Their)->GetSchema()->GetClass());
			Graph->GraphGuid = (*Their)->GraphGuid;
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Ours, Graph, /*bIsUserCreated=*/true, nullptr);

			const UK2Node_FunctionEntry* TheirEntry = nullptr;
			for (const UEdGraphNode* Node : (*Their)->Nodes)
			{
				if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
					TheirEntry = Entry;
			}

			UK2Node_FunctionEntry* MyEntry = nullptr;
			for (UEdGraphNode* Node : TArray<UEdGraphNode*>(Graph->Nodes))
			{
				if (!MyEntry && Node && Node->IsA<UK2Node_FunctionEntry>())
					MyEntry = CastChecked<UK2Node_FunctionEntry>(Node);
				else if (Node)
					FBlueprintEditorUtils::RemoveNode(Ours, Node, /*bDontRecompile=*/true);
			}

			if (!MyEntry || !TheirEntry)
			{
				OutProblems.Add(FString::Printf(TEXT("Could not set up the function '%s'."), *Name));
				continue;
			}

			MyEntry->NodeGuid = TheirEntry->NodeGuid;
			SyncTerminator(MyEntry, TheirEntry);
			NodesInPlace.Add(TheirEntry->NodeGuid.ToString());
		}

		const TMap<FString, UEdGraph*> OurGraphs = GraphsById(Ours);

		// 4. Copy nodes theirs added (or reconfigured), keeping their guids so links can find them.
		for (const TArray<FString>* List : {&NodesToAdd, &NodesToReplace})
		{
			for (const FString& Id : *List)
			{
				if (NodesInPlace.Contains(Id))
					continue;

				UEdGraphNode* const* TheirNode = TheirNodes.Find(Id);
				if (!TheirNode)
				{
					OutProblems.Add(FString::Printf(TEXT("Node %s is missing from theirs."), *Id));
					continue;
				}

				UEdGraph* const* Graph = OurGraphs.Find(GraphId((*TheirNode)->GetGraph()));
				if (!Graph)
				{
					OutProblems.Add(FString::Printf(TEXT("Graph '%s' only exists in theirs; adding graphs is not supported yet."), *(*TheirNode)->GetGraph()->GetName()));
					continue;
				}

				FString Text;
				FEdGraphUtilities::ExportNodesToText(TSet<UObject*>{*TheirNode}, Text);

				TSet<UEdGraphNode*> Imported;
				FEdGraphUtilities::ImportNodesFromText(*Graph, Retarget(Text), Imported);
				if (Imported.Num() != 1)
				{
					OutProblems.Add(FString::Printf(TEXT("Could not copy node %s from theirs."), *Id));
					continue;
				}

				for (UEdGraphNode* Node : Imported)
					Node->NodeGuid = (*TheirNode)->NodeGuid;
			}
		}

		const TMap<FString, UEdGraphNode*> OurNodes = NodesById(Ours);

		for (const FString& Id : TerminatorsToSync)
		{
			UK2Node_FunctionTerminator* Mine = Cast<UK2Node_FunctionTerminator>(OurNodes.FindRef(Id));
			const UK2Node_FunctionTerminator* Their = Cast<UK2Node_FunctionTerminator>(TheirNodes.FindRef(Id));
			if (Mine && Their)
				SyncTerminator(Mine, Their);
			else
				OutProblems.Add(FString::Printf(TEXT("Could not update function node %s."), *Id));
		}

		// 5. Small edits on nodes both sides kept.
		TSet<UEdGraphNode*> NodesToReconstruct;
		for (const FDecision* Decision : NodeEdits)
		{
			FString Id, Detail;
			SplitEntityKey(Decision->Key, 5, Id, Detail);
			if (NodesToReplace.Contains(Id))
				continue;

			UEdGraphNode* const* Mine = OurNodes.Find(Id);
			UEdGraphNode* const* Their = TheirNodes.Find(Id);
			if (!Mine || !Their)
			{
				OutProblems.Add(FString::Printf(TEXT("Could not apply %s."), *Decision->Key));
				continue;
			}

			if (Detail == TEXT("pos"))
			{
				(*Mine)->NodePosX = (*Their)->NodePosX;
				(*Mine)->NodePosY = (*Their)->NodePosY;
			}
			else if (Detail == TEXT("comment"))
			{
				(*Mine)->NodeComment = (*Their)->NodeComment;
				(*Mine)->bCommentBubbleVisible = (*Their)->bCommentBubbleVisible;
				(*Mine)->bCommentBubblePinned = (*Their)->bCommentBubblePinned;
			}
			else if (Detail == TEXT("enabled"))
			{
				(*Mine)->SetEnabledState((*Their)->GetDesiredEnabledState(), (*Their)->HasUserSetTheEnabledState());
			}
			else if (Detail.StartsWith(TEXT("prop/")))
			{
				if (CopySetting(*Mine, *Their, Detail.Mid(5), Retarget))
					NodesToReconstruct.Add(*Mine);
				else
					OutProblems.Add(FString::Printf(TEXT("Could not apply %s."), *Decision->Key));
			}
		}

		// Pins may depend on a node's settings.
		for (UEdGraphNode* Node : NodesToReconstruct)
			Node->ReconstructNode();

		// 5a. Widgets theirs added, panels before what goes into them; then settings of widgets both sides have.
		if (WidgetsToAdd.Num() > 0 || WidgetEdits.Num() > 0)
		{
			TMap<FString, UObject*> MyInner = InnerObjects(Ours);
			const TMap<FString, UObject*> TheirInner = InnerObjects(Theirs);
			UWidgetTree* MyTree = Cast<UWidgetTree>(MyInner.FindRef(TEXT("WidgetTree")));
			TArray<const FDecision*> WidgetsInOrder;

			// Widgets all sit directly in the tree, so a panel theirs added comes first only by taking it in rounds.
			TArray<const FDecision*> Pending = WidgetsToAdd;
			for (bool bProgress = true; bProgress && Pending.Num() > 0;)
			{
				bProgress = false;
				for (int32 Index = 0; Index < Pending.Num(); ++Index)
				{
					const UWidget* Their = Cast<UWidget>(TheirInner.FindRef(Pending[Index]->Key.Mid(6)));
					const UPanelWidget* TheirParent = Their ? Their->GetParent() : nullptr;
					if (!TheirParent || MyInner.Contains(TheirParent->GetPathName(Theirs)))
					{
						WidgetsInOrder.Add(Pending[Index]);
						Pending.RemoveAt(Index--);
						bProgress = true;
					}
				}
			}
			WidgetsInOrder.Append(Pending);

			for (const FDecision* Decision : WidgetsInOrder)
			{
				const FString Path = Decision->Key.Mid(6);
				const UWidget* Their = Cast<UWidget>(TheirInner.FindRef(Path));
				const UPanelWidget* TheirParent = Their ? Their->GetParent() : nullptr;
				UPanelWidget* MyParent = TheirParent ? Cast<UPanelWidget>(MyInner.FindRef(TheirParent->GetPathName(Theirs))) : nullptr;
				if (!MyTree || !Their || !MyParent || !Their->Slot)
				{
					OutProblems.Add(FString::Printf(TEXT("Could not add widget '%s' from theirs."), *Path));
					continue;
				}
				if (MyTree->FindWidget(Their->GetFName()) || FBlueprintEditorUtils::FindNewVariableIndex(Ours, Their->GetFName()) != INDEX_NONE)
				{
					OutProblems.Add(FString::Printf(TEXT("Theirs added the widget '%s', but ours already has something with that name."), *Their->GetName()));
					continue;
				}

				MyTree->Modify();
				MyParent->Modify();
				UWidget* Mine = NewObject<UWidget>(MyTree, Their->GetClass(), Their->GetFName(), RF_Transactional);
				const bool bWidget = CopyAllSettings(Mine, Their, {TEXT("Slot"), TEXT("Slots")}, Retarget);
				UPanelSlot* Slot = MyParent->InsertChildAt(FMath::Min(TheirParent->GetChildIndex(Their), MyParent->GetChildrenCount()), Mine);
				const bool bSlot = Slot && CopyAllSettings(Slot, Their->Slot, {TEXT("Content"), TEXT("Parent")}, Retarget);
				if (!bWidget || !bSlot)
					OutProblems.Add(FString::Printf(TEXT("Could not copy all settings of widget '%s' from theirs."), *Their->GetName()));
				if (Slot)
					Slot->SynchronizeProperties();
				CopyWidgetGuid(Ours, Theirs, Their->GetFName());
				MyInner.Add(Path, Mine);
			}

			for (const FDecision* Decision : WidgetEdits)
			{
				// "inner/<path>/<property>" or "inner/<widget>/Slot.<property>"
				FString Path, Property;
				Decision->Key.Mid(6).Split(TEXT("/"), &Path, &Property, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				UObject* Mine = MyInner.FindRef(Path);
				const UObject* Their = TheirInner.FindRef(Path);
				if (Property.RemoveFromStart(TEXT("Slot.")))
				{
					Mine = Cast<UWidget>(Mine) ? Cast<UWidget>(Mine)->Slot.Get() : nullptr;
					Their = Cast<UWidget>(Their) ? Cast<UWidget>(Their)->Slot.Get() : nullptr;
				}

				if (!Mine || !Their || !CopySetting(Mine, Their, Property, Retarget))
				{
					OutProblems.Add(FString::Printf(TEXT("Could not set '%s' of '%s'."), *Property, *Path));
					continue;
				}
				if (UPanelSlot* Slot = Cast<UPanelSlot>(Mine))
					Slot->SynchronizeProperties();
			}
		}

		// 5b. Class Settings.
		for (const FDecision* Decision : ClassSettings)
		{
			if (!CopySetting(Ours, Theirs, Decision->Key.Mid(3), Retarget))
				OutProblems.Add(FString::Printf(TEXT("Could not set class setting '%s'."), *Decision->Key.Mid(3)));
		}

		for (const FDecision* Decision : PinEdits)
		{
			FString Id, Key;
			SplitPinRef(Decision->Key.Mid(4), Id, Key);
			if (NodesToReplace.Contains(Id))
				continue;

			UEdGraphNode* const* Mine = OurNodes.Find(Id);
			UEdGraphNode* const* Their = TheirNodes.Find(Id);
			UEdGraphPin* MyPin = Mine ? FindPin(*Mine, Key) : nullptr;
			const UEdGraphPin* TheirPin = Their ? FindPin(*Their, Key) : nullptr;
			if (!MyPin || !TheirPin)
			{
				OutProblems.Add(FString::Printf(TEXT("Could not set pin %s."), *Decision->Key));
				continue;
			}

			MyPin->DefaultValue = Retarget(TheirPin->DefaultValue);
			MyPin->DefaultObject = Retarget(TheirPin->DefaultObject);
			MyPin->DefaultTextValue = TheirPin->DefaultTextValue;
		}

		// 6. Make every connection match the merged result. Copied nodes come in unconnected.
		for (const FDecision& Decision : Plan.Decisions)
		{
			FString Src, Dst;
			if (!SplitLink(Decision.Key, Src, Dst))
				continue;

			FString SrcNode, SrcPin, DstNode, DstPin;
			SplitPinRef(Src, SrcNode, SrcPin);
			SplitPinRef(Dst, DstNode, DstPin);

			UEdGraphNode* const* From = OurNodes.Find(SrcNode);
			UEdGraphNode* const* To = OurNodes.Find(DstNode);
			UEdGraphPin* FromPin = From ? FindPin(*From, SrcPin) : nullptr;
			UEdGraphPin* ToPin = To ? FindPin(*To, DstPin) : nullptr;

			const bool bWanted = MergedValue(Decision).IsSet();
			if (!FromPin || !ToPin)
			{
				if (bWanted)
					OutProblems.Add(FString::Printf(TEXT("Could not find the pins of connection %s."), *Decision.Key));
				continue;
			}

			const bool bLinked = FromPin->LinkedTo.Contains(ToPin);
			if (bWanted && !bLinked)
			{
				if (!FromPin->GetSchema()->TryCreateConnection(FromPin, ToPin))
					OutProblems.Add(FString::Printf(TEXT("Could not connect %s."), *Decision.Key));
			}
			else if (!bWanted && bLinked)
			{
				FromPin->GetSchema()->BreakSinglePinLink(FromPin, ToPin);
			}
		}

		// 6b. Settings of components both sides have, copied from theirs' component template.
		for (const FDecision* Decision : ComponentEdits)
		{
			FString Id, Detail;
			SplitEntityKey(Decision->Key, 10, Id, Detail);
			const FName Name(*Detail.Mid(5)); // "prop/<name>"

			const USCS_Node* MyNode = FindComponent(Ours, Id);
			const USCS_Node* TheirNode = FindComponent(Theirs, Id);
			UObject* MyTemplate = MyNode ? MyNode->ComponentTemplate.Get() : nullptr;
			const UObject* TheirTemplate = TheirNode ? TheirNode->ComponentTemplate.Get() : nullptr;
			const FProperty* MyProperty = MyTemplate ? FindFProperty<FProperty>(MyTemplate->GetClass(), Name) : nullptr;
			const FProperty* TheirProperty = TheirTemplate ? FindFProperty<FProperty>(TheirTemplate->GetClass(), Name) : nullptr;
			if (!MyProperty || !TheirProperty)
			{
				OutProblems.Add(FString::Printf(TEXT("Could not set '%s' of a component."), *Name.ToString()));
				continue;
			}

			FString Value;
			TheirProperty->ExportTextItem_InContainer(Value, TheirTemplate, nullptr, nullptr, PPF_None);
			MyTemplate->Modify();
			if (!MyProperty->ImportText_InContainer(*Retarget(Value), MyTemplate, MyTemplate, PPF_None))
				OutProblems.Add(FString::Printf(TEXT("Could not set '%s' of a component."), *Name.ToString()));
		}

		if (OutProblems.Num() > 0)
			return false;

		// 7. Compile; a merge that doesn't compile is not a merge.
		if (!CompileMerged(Ours, OutProblems))
			return false;

		// 8. Class defaults, after compiling so variables theirs added exist on the class.
		UObject* MyDefaults = Ours->GeneratedClass ? Ours->GeneratedClass->GetDefaultObject() : nullptr;
		UObject* TheirDefaults = Theirs->GeneratedClass ? Theirs->GeneratedClass->GetDefaultObject() : nullptr;
		for (const FDecision* Decision : ClassDefaults)
		{
			const FName Name(*Decision->Key.Mid(4));
			const FProperty* MyProperty = MyDefaults ? FindFProperty<FProperty>(MyDefaults->GetClass(), Name) : nullptr;
			const FProperty* TheirProperty = TheirDefaults ? FindFProperty<FProperty>(TheirDefaults->GetClass(), Name) : nullptr;
			if (!MyProperty || !TheirProperty)
			{
				OutProblems.Add(FString::Printf(TEXT("Could not set class default '%s'."), *Name.ToString()));
				continue;
			}

			FString Value;
			TheirProperty->ExportTextItem_InContainer(Value, TheirDefaults, nullptr, nullptr, PPF_None);
			if (!MyProperty->ImportText_InContainer(*Retarget(Value), MyDefaults, MyDefaults, PPF_None))
				OutProblems.Add(FString::Printf(TEXT("Could not set class default '%s'."), *Name.ToString()));
		}

		return OutProblems.Num() == 0;
	}

	bool CompileMerged(UBlueprint* Blueprint, TArray<FString>& OutProblems)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
		if (Results.NumErrors == 0 && Blueprint->Status != BS_Error)
			return true;

		const int32 Before = OutProblems.Num();
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			if (Message->GetSeverity() == EMessageSeverity::Error)
				OutProblems.Add(TEXT("Compile error: ") + Message->ToText().ToString());
		}
		if (OutProblems.Num() == Before)
			OutProblems.Add(TEXT("The merged Blueprint does not compile."));
		return false;
	}

	TArray<UEdGraphNode*> CopyNodes(const TArray<UEdGraphNode*>& Nodes, UBlueprint* Source, UBlueprint* Target, FString& OutProblem, const FVector2D* DropAt)
	{
		const FRetarget Retarget(Source, Target);
		const TMap<FString, UEdGraphNode*> TargetNodes = NodesById(Target);

		TMap<FString, UEdGraph*> TargetGraphs;
		TArray<UEdGraph*> Graphs;
		Target->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph)
				TargetGraphs.Add(GraphId(Graph), Graph);
		}

		TMap<FString, UEdGraphNode*> SourceById;
		for (UEdGraphNode* Node : Nodes)
		{
			if (Node)
				SourceById.Add(Node->NodeGuid.ToString(), Node);
		}

		// One paste per graph, so wires between the copied nodes are kept.
		TMap<UEdGraph*, TSet<UObject*>> ByGraph;
		for (UEdGraphNode* Node : Nodes)
		{
			UEdGraph* const* Graph = Node ? TargetGraphs.Find(GraphId(Node->GetGraph())) : nullptr;
			if (!Graph)
			{
				OutProblem = TEXT("Some nodes are in a graph the result doesn't have; add that graph by hand first.");
				continue;
			}
			if (!Node->CanDuplicateNode())
			{
				OutProblem = FString::Printf(TEXT("'%s' can't be copied."), *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				continue;
			}
			ByGraph.FindOrAdd(*Graph).Add(Node);
		}

		TArray<UEdGraphNode*> Copies;
		TMap<UEdGraphNode*, UEdGraphNode*> SourceOf;
		for (const TPair<UEdGraph*, TSet<UObject*>>& Pair : ByGraph)
		{
			FString Text;
			FEdGraphUtilities::ExportNodesToText(Pair.Value, Text);

			Pair.Key->Modify();
			TSet<UEdGraphNode*> Imported;
			FEdGraphUtilities::ImportNodesFromText(Pair.Key, Retarget(Text), Imported);
			for (UEdGraphNode* Node : Imported)
			{
				if (UEdGraphNode* const* From = SourceById.Find(Node->NodeGuid.ToString()))
					SourceOf.Add(Node, *From);

				// Already in the result: this is a second copy, so it needs its own id and a spot of its own.
				if (TargetNodes.Contains(Node->NodeGuid.ToString()))
				{
					Node->CreateNewGuid();
					Node->NodePosX += 40;
					Node->NodePosY += 40;
				}
				Copies.Add(Node);
			}
		}

		// Dropped somewhere: keep the copies' layout, with their top-left corner where they were dropped.
		if (DropAt && Copies.Num() > 0)
		{
			int32 MinX = TNumericLimits<int32>::Max();
			int32 MinY = TNumericLimits<int32>::Max();
			for (const UEdGraphNode* Node : Copies)
			{
				MinX = FMath::Min(MinX, Node->NodePosX);
				MinY = FMath::Min(MinY, Node->NodePosY);
			}
			for (UEdGraphNode* Node : Copies)
			{
				Node->NodePosX += FMath::RoundToInt32(DropAt->X) - MinX;
				Node->NodePosY += FMath::RoundToInt32(DropAt->Y) - MinY;
			}
		}

		// Wires to nodes outside the copy: connect them to the same nodes in the result, when the result has them. A pin
		// that only takes one wire and already has one is left alone, so nothing already in the result gets rewired.
		TArray<FString> NotConnected;
		for (const TPair<UEdGraphNode*, UEdGraphNode*>& Pair : SourceOf)
		{
			UEdGraphNode* Copy = Pair.Key;
			const UEdGraphSchema* Schema = Copy->GetGraph()->GetSchema();
			for (const UEdGraphPin* FromPin : Pair.Value->Pins)
			{
				for (const UEdGraphPin* Other : FromPin->LinkedTo)
				{
					const UEdGraphNode* OtherNode = Other ? Other->GetOwningNode() : nullptr;
					if (!OtherNode || SourceById.Contains(OtherNode->NodeGuid.ToString()))
						continue;

					UEdGraphNode* const* Mine = TargetNodes.Find(OtherNode->NodeGuid.ToString());
					if (!Mine || (*Mine)->GetGraph() != Copy->GetGraph())
						continue;

					UEdGraphPin* A = Copy->FindPin(FromPin->PinName, FromPin->Direction);
					UEdGraphPin* B = (*Mine)->FindPin(Other->PinName, Other->Direction);
					if (!A || !B || A->LinkedTo.Contains(B))
						continue;

					if (Schema->CanCreateConnection(A, B).Response == CONNECT_RESPONSE_MAKE)
					{
						Schema->TryCreateConnection(A, B);
					}
					else
					{
						const UEdGraphPin* Out = A->Direction == EGPD_Output ? A : B;
						const UEdGraphPin* In = A->Direction == EGPD_Output ? B : A;
						NotConnected.Add(FString::Printf(TEXT("'%s' -> '%s'"),
							*Out->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString(), *In->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString()));
					}
				}
			}
		}
		if (NotConnected.Num() > 0)
		{
			const FString Text = TEXT("Not wired, because that pin in the result already has a wire (connect by hand if you want it): ") + FString::Join(NotConnected, TEXT(", "));
			OutProblem = OutProblem.IsEmpty() ? Text : OutProblem + TEXT(" ") + Text;
		}

		if (Copies.Num() > 0)
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Target);
		return Copies;
	}
}
