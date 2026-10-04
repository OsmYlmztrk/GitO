// Copyright (c) MyGitGUI contributors. MIT License.

#include "BlueprintMergeApply.h"

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
		TArray<const FDecision*> NodeEdits, PinEdits, VariableChanges, ClassDefaults, GraphChanges, ComponentEdits;

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
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ours);
		FCompilerResultsLog Results;
		FKismetEditorUtilities::CompileBlueprint(Ours, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
		if (Results.NumErrors > 0 || Ours->Status == BS_Error)
		{
			for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
			{
				if (Message->GetSeverity() == EMessageSeverity::Error)
					OutProblems.Add(TEXT("Compile error: ") + Message->ToText().ToString());
			}
			if (OutProblems.Num() == 0)
				OutProblems.Add(TEXT("The merged Blueprint does not compile."));
			return false;
		}

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
}
