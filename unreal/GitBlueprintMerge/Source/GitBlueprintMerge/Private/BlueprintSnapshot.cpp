// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#include "BlueprintSnapshot.h"

#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_Variable.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace GitBlueprintMerge
{
	FString PinKey(const UEdGraphPin* Pin)
	{
		return FString::Printf(TEXT("%s:%s"), Pin->Direction == EGPD_Output ? TEXT("out") : TEXT("in"), *Pin->PinName.ToString());
	}

	FString GraphId(const UEdGraph* Graph)
	{
		return Graph->GraphGuid.IsValid() ? Graph->GraphGuid.ToString() : Graph->GetName();
	}

	bool SplitLink(const FString& Key, FString& Src, FString& Dst)
	{
		return Key.StartsWith(TEXT("link/")) && Key.Mid(5).Split(TEXT("|"), &Src, &Dst);
	}

	void SplitPinRef(const FString& Ref, FString& Node, FString& Pin)
	{
		Ref.Split(TEXT(":"), &Node, &Pin);
	}

	const TOptional<FString>& MergedValue(const FDecision& Decision)
	{
		return Decision.Resolution == EResolution::Theirs ? Decision.Theirs : Decision.Ours;
	}

	namespace
	{
		FString PinRef(const UEdGraphPin* Pin)
		{
			return Pin->GetOwningNode()->NodeGuid.ToString() + TEXT(":") + PinKey(Pin);
		}

		FString PinDefault(const UEdGraphPin* Pin)
		{
			if (Pin->DefaultObject)
				return Pin->DefaultObject->GetPathName();
			if (!Pin->DefaultTextValue.IsEmpty())
				return Pin->DefaultTextValue.ToString();
			return Pin->DefaultValue;
		}

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

		void AddProperties(FSnapshot& Snap, const FString& Prefix, const UObject* Object, const FNormalizer& Normalize)
		{
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				const FProperty* Property = *It;
				if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
					continue;

				FString Value;
				Property->ExportTextItem_InContainer(Value, Object, nullptr, nullptr, PPF_None);
				Snap.Facts.Add(Prefix + Property->GetName(), Normalize(Value));
			}
		}

		bool IsSkipped(const FProperty* Property)
		{
			return Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated);
		}

		/** Values that can't be copied as text: instanced objects (the copy would point at theirs' object). */
		bool HoldsInstancedObjects(const FProperty* Property)
		{
			return Property->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance);
		}

		/** Every element of a fixed-size array, otherwise the one value. */
		FString ExportValue(const FProperty* Property, const void* Container)
		{
			FString Text;
			for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
			{
				if (Index > 0)
					Text += TEXT(",");
				Property->ExportTextItem_Direct(Text, Property->ContainerPtrToValuePtr<void>(Container, Index), nullptr, nullptr, PPF_None);
			}
			return Text;
		}

		/**
		 * Class Settings of the Blueprint ("bp/<property>"), merged by copying the value. A Widget or Anim Blueprint's
		 * own data (widget tree, animations, bindings, groups, ...) is compared too but can't be merged yet.
		 * Values Unreal works out itself when compiling (searchable in the asset registry, not editable) are left out.
		 */
		void AddBlueprintSettings(FSnapshot& Snap, const UBlueprint* Blueprint, const FNormalizer& Normalize)
		{
			for (TFieldIterator<FProperty> It(Blueprint->GetClass()); It; ++It)
			{
				const FProperty* Property = *It;
				const UClass* Owner = Property->GetOwnerClass();
				const bool bOwnData = Owner != UBlueprint::StaticClass() && Owner->IsChildOf(UBlueprint::StaticClass());
				const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit);
				if (IsSkipped(Property) || Property->GetFName() == TEXT("ParentClass") || Property->GetFName() == TEXT("WidgetVariableNameToGuidMap") || (!bEditable && (!bOwnData || Property->HasAnyPropertyFlags(CPF_AssetRegistrySearchable))))
					continue;

				const FString Key = TEXT("bp/") + Property->GetName();
				Snap.Facts.Add(Key, Normalize(ExportValue(Property, Blueprint)));
				Snap.Labels.Add(Key, Property->GetDisplayNameText().ToString());
				if (!bEditable || Property->ArrayDim > 1 || HoldsInstancedObjects(Property))
					Snap.Unmergeable.Add(Key);
			}
		}

		/**
		 * Widget and slot settings that can be copied from theirs as text. Not: instanced objects, function bindings
		 * (delegates), the widget's link to its slot.
		 */
		bool IsCopyableWidgetProperty(const FProperty* Property)
		{
			return Property->ArrayDim == 1 && !HoldsInstancedObjects(Property) && !Property->IsA<FDelegateProperty>() &&
				!Property->IsA<FMulticastDelegateProperty>() && Property->GetFName() != TEXT("Slot");
		}

		/** Editor-only view state that changes by just looking at an asset. */
		bool IsViewState(const FProperty* Property)
		{
			static const FName Names[] = {TEXT("bExpandedInDesigner"), TEXT("EditorData")};
			for (const FName& Name : Names)
			{
				if (Property->GetFName() == Name)
					return true;
			}
			return false;
		}

		bool IsClassNamed(const UClass* Class, const TCHAR* Name)
		{
			for (; Class; Class = Class->GetSuperClass())
			{
				if (Class->GetFName() == Name)
					return true;
			}
			return false;
		}

		/**
		 * True for objects the snapshot already covers (graphs and their nodes, the components) or that the compiler
		 * makes (Anim Blueprint extensions).
		 */
		bool IsCovered(const UObject* Object, const UBlueprint* Blueprint)
		{
			for (const UObject* Current = Object; Current && Current != Blueprint; Current = Current->GetOuter())
			{
				if (Current->IsA<UEdGraph>() || Current->IsA<UEdGraphNode>() || Current->IsA<USimpleConstructionScript>() || Current->IsA<USCS_Node>() ||
					IsClassNamed(Current->GetClass(), TEXT("AnimBlueprintExtension")))
					return true;
			}
			return false;
		}

		/**
		 * Everything else the Blueprint owns ("inner/<path>", "inner/<path>/<property>"): the widget tree and widget
		 * animations of a Widget Blueprint, inherited component overrides, ... Compared so theirs' changes there are
		 * listed instead of lost, but not merged yet.
		 */
		void AddInnerObjects(FSnapshot& Snap, const UBlueprint* Blueprint, const FNormalizer& Normalize)
		{
			TArray<UObject*> Inner;
			GetObjectsWithOuter(Blueprint, Inner, EGetObjectsFlags::IncludeNestedObjects);
			for (const UObject* Object : Inner)
			{
				if (Object->HasAnyFlags(RF_Transient) || IsCovered(Object, Blueprint))
					continue;

				// A panel slot is where its widget sits (anchors, padding, ...): shown as "Slot ..." settings of
				// that widget, so adding a widget is one change and not three.
				const bool bSlot = IsClassNamed(Object->GetClass(), TEXT("PanelSlot"));
				const FObjectProperty* ContentProperty = bSlot ? FindFProperty<FObjectProperty>(Object->GetClass(), TEXT("Content")) : nullptr;
				const UObject* Content = ContentProperty ? ContentProperty->GetObjectPropertyValue_InContainer(Object) : nullptr;
				const bool bSlotOfWidget = Content && Content->IsIn(Blueprint);

				const FString Key = TEXT("inner/") + (bSlotOfWidget ? Content : Object)->GetPathName(Blueprint);
				const FString Prefix = bSlotOfWidget ? TEXT("Slot.") : TEXT("");
				const UWidget* Widget = Cast<UWidget>(Object);
				if (!bSlotOfWidget)
				{
					Snap.Facts.Add(Key, Object->GetClass()->GetPathName());
					Snap.Labels.Add(Key, TEXT("'") + Object->GetName() + TEXT("' (") + Object->GetClass()->GetDisplayNameText().ToString() + TEXT(")"));
					if (Widget)
					{
						const UPanelWidget* Parent = Widget->GetParent();
						Snap.WidgetParents.Add(Key, Parent && Parent->IsIn(Blueprint) ? TEXT("inner/") + Parent->GetPathName(Blueprint) : FString());
					}
					else
					{
						Snap.Unmergeable.Add(Key);
					}
				}

				for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
				{
					// A panel's slot list and a slot's links back to its panel and widget follow the tree itself.
					const bool bTreeLink = (bSlotOfWidget && (It->GetFName() == TEXT("Content") || It->GetFName() == TEXT("Parent")))
						|| (It->GetFName() == TEXT("Slots") && IsClassNamed(Object->GetClass(), TEXT("PanelWidget")));
					if (IsSkipped(*It) || IsViewState(*It) || bTreeLink)
						continue;

					const FString PropertyKey = Key + TEXT("/") + Prefix + It->GetName();
					Snap.Facts.Add(PropertyKey, Normalize(ExportValue(*It, Object)));
					Snap.Labels.Add(PropertyKey, (bSlotOfWidget ? TEXT("Slot ") : TEXT("")) + It->GetDisplayNameText().ToString());
					if (!((Widget || bSlotOfWidget) && IsCopyableWidgetProperty(*It)))
						Snap.Unmergeable.Add(PropertyKey);
				}
			}
		}

		/**
		 * A node's own settings from the details panel ("node/<node>/prop/<property>[.<field>]"), e.g. the settings of
		 * an Anim Graph node or a state transition. Struct settings are split into fields; fields shown as pins are
		 * left to the pin facts.
		 */
		void AddNodeSettings(FSnapshot& Snap, const FString& NodeKey, const UEdGraphNode* Node, const FNormalizer& Normalize)
		{
			// Function entry/result nodes are synced as a whole.
			if (Node->IsA<UK2Node_FunctionTerminator>())
				return;

			auto Add = [&](const FString& Path, const FString& Label, const FProperty* Property, const void* Container, bool bMergeable = true)
			{
				const FString Key = NodeKey + TEXT("/prop/") + Path;
				Snap.Facts.Add(Key, Normalize(ExportValue(Property, Container)));
				Snap.Labels.Add(Key, Label);
				if (!bMergeable || Property->ArrayDim > 1 || HoldsInstancedObjects(Property))
					Snap.Unmergeable.Add(Key);
			};

			for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
			{
				const FProperty* Property = *It;
				if (!Property->HasAnyPropertyFlags(CPF_Edit) || IsSkipped(Property) || Property->GetOwnerClass() == UEdGraphNode::StaticClass() || Node->FindPin(Property->GetFName()))
					continue;

				// Instanced subobjects of a node (e.g. an Anim Graph node's property bindings): compared by content.
				const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);
				const UObject* Subobject = ObjectProperty && Property->ArrayDim == 1 && HoldsInstancedObjects(Property) ? ObjectProperty->GetObjectPropertyValue_InContainer(Node) : nullptr;
				if (Subobject)
				{
					for (TFieldIterator<FProperty> Field(Subobject->GetClass()); Field; ++Field)
					{
						if (!IsSkipped(*Field))
							Add(Property->GetName() + TEXT(".") + Field->GetName(), Property->GetDisplayNameText().ToString() + TEXT(": ") + Field->GetDisplayNameText().ToString(), *Field, Subobject, false);
					}
					continue;
				}

				const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
				const bool bSplit = StructProperty && Property->ArrayDim == 1 && !(StructProperty->Struct->StructFlags & (STRUCT_ImportTextItemNative | STRUCT_ExportTextItemNative));
				if (!bSplit)
				{
					Add(Property->GetName(), Property->GetDisplayNameText().ToString(), Property, Node);
					continue;
				}

				const void* Value = Property->ContainerPtrToValuePtr<void>(Node);
				for (TFieldIterator<FProperty> Field(StructProperty->Struct); Field; ++Field)
				{
					if (Field->HasAnyPropertyFlags(CPF_Edit) && !IsSkipped(*Field) && !Node->FindPin(Field->GetFName()))
						Add(Property->GetName() + TEXT(".") + Field->GetName(), Field->GetDisplayNameText().ToString(), *Field, Value);
				}
			}
		}

		TOptional<FString> FindFact(const FSnapshot& Snap, const FString& Key)
		{
			const FString* Value = Snap.Facts.Find(Key);
			return Value ? TOptional<FString>(*Value) : TOptional<FString>();
		}

		/** The entities (node/var/component keys) a fact belongs to. Links belong to both of their nodes. */
		TArray<FString, TInlineAllocator<2>> OwnersOf(const FString& Key)
		{
			TArray<FString, TInlineAllocator<2>> Owners;

			FString Src, Dst;
			if (SplitLink(Key, Src, Dst))
			{
				FString Node, Pin;
				SplitPinRef(Src, Node, Pin);
				Owners.Add(TEXT("node/") + Node);
				SplitPinRef(Dst, Node, Pin);
				Owners.Add(TEXT("node/") + Node);
				return Owners;
			}

			if (Key.StartsWith(TEXT("pin/")))
			{
				FString Node, Pin;
				SplitPinRef(Key.Mid(4), Node, Pin);
				Owners.Add(TEXT("node/") + Node);
				return Owners;
			}

			for (const TCHAR* Prefix : {TEXT("node/"), TEXT("var/"), TEXT("component/"), TEXT("row/"), TEXT("inner/")})
			{
				if (Key.StartsWith(Prefix))
				{
					const int32 Slash = Key.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FCString::Strlen(Prefix));
					if (Slash != INDEX_NONE)
						Owners.Add(Key.Left(Slash));
					break;
				}
			}

			return Owners;
		}

		bool IsEntityKey(const FString& Key)
		{
			return (Key.StartsWith(TEXT("node/")) || Key.StartsWith(TEXT("var/")) || Key.StartsWith(TEXT("component/")) || Key.StartsWith(TEXT("row/")) || Key.StartsWith(TEXT("inner/")))
				&& OwnersOf(Key).Num() == 0;
		}

		/** "Name|Type|Category|Flags" of a variable fact -> one of its fields. */
		FString VariableField(const TOptional<FString>& Value, int32 Field)
		{
			TArray<FString> Fields;
			if (Value.IsSet())
				Value->ParseIntoArray(Fields, TEXT("|"), false);
			return Fields.IsValidIndex(Field) ? Fields[Field] : FString();
		}

		/** "2.000000" -> "2.0", "(X=0.200000,Y=1.000000)" -> "(X=0.2,Y=1.0)": how Unreal exports floats is hard to read. */
		FString TrimZeros(const FString& Text)
		{
			FString Out;
			int32 Index = 0;
			while (Index < Text.Len())
			{
				if (!FChar::IsDigit(Text[Index]))
				{
					Out.AppendChar(Text[Index++]);
					continue;
				}

				const int32 Start = Index;
				while (Index < Text.Len() && FChar::IsDigit(Text[Index]))
					++Index;

				if (Index + 1 < Text.Len() && Text[Index] == TEXT('.') && FChar::IsDigit(Text[Index + 1]))
				{
					const int32 Fraction = Index + 1;
					Index = Fraction;
					while (Index < Text.Len() && FChar::IsDigit(Text[Index]))
						++Index;

					int32 End = Index;
					while (End > Fraction + 1 && Text[End - 1] == TEXT('0'))
						--End;
					Out += Text.Mid(Start, End - Start);
				}
				else
				{
					Out += Text.Mid(Start, Index - Start);
				}
			}
			return Out;
		}

		/** NSLOCTEXT("", "<key>", "Hello") -> Hello: the key changes with every edit and means nothing to people. */
		FString ReadableText(const FString& Value)
		{
			if (!Value.StartsWith(TEXT("NSLOCTEXT(")) || !Value.EndsWith(TEXT("\")")))
				return Value;

			int32 Found = 0;
			for (int32 Index = 0; Index + 4 <= Value.Len(); ++Index)
			{
				if (Value.Mid(Index, 4) == TEXT("\", \"") && ++Found == 2)
					return Value.Mid(Index + 4, Value.Len() - 2 - (Index + 4));
			}
			return Value;
		}

		/**
		 * "/Script/UMG.CanvasPanelSlot'<self>.WBP_Menu:WidgetTree.Root.CanvasPanelSlot_0'" -> "CanvasPanelSlot_0":
		 * references to objects inside the asset, by name.
		 */
		FString ShortObjectNames(const FString& Value)
		{
			FString Out = Value;
			for (int32 Quote = Out.Find(TEXT("'<self>.")); Quote != INDEX_NONE; Quote = Out.Find(TEXT("'<self>.")))
			{
				const int32 End = Out.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Quote + 1);
				if (End == INDEX_NONE)
					break;

				int32 Start = Quote;
				while (Start > 0 && !FChar::IsWhitespace(Out[Start - 1]) && FCString::Strchr(TEXT("\"(,="), Out[Start - 1]) == nullptr)
					--Start;

				const FString Path = Out.Mid(Quote + 1, End - Quote - 1);
				int32 Cut = INDEX_NONE;
				for (int32 Index = Path.Len() - 1; Index >= 0 && Cut == INDEX_NONE; --Index)
				{
					if (Path[Index] == TEXT('.') || Path[Index] == TEXT(':'))
						Cut = Index;
				}
				Out = Out.Left(Start) + Path.Mid(Cut + 1) + Out.Mid(End + 1);
			}
			return Out;
		}

		FString Shorten(const TOptional<FString>& Value)
		{
			if (!Value.IsSet())
				return TEXT("(none)");
			FString Text = Value.GetValue().Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" "));
			if (Text.Len() > 100)
				Text = Text.Left(100) + TEXT("...");
			return TEXT("'") + Text + TEXT("'");
		}
	}

	FSnapshot TakeSnapshot(const UBlueprint* Blueprint)
	{
		FSnapshot Snap;
		const FNormalizer Normalize(Blueprint);

		Snap.Facts.Add(TEXT("parent"), GetPathNameSafe(Blueprint->ParentClass));

		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
			Snap.Facts.Add(TEXT("interface/") + GetPathNameSafe(Interface.Interface), TEXT("1"));

		for (const FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			const FString Id = Var.VarGuid.ToString();
			Snap.Facts.Add(TEXT("var/") + Id, FString::Printf(TEXT("%s|%s|%s|%llu"),
				*Var.VarName.ToString(), *UEdGraphSchema_K2::TypeToText(Var.VarType).ToString(), *Var.Category.ToString(), Var.PropertyFlags));
			Snap.Facts.Add(TEXT("var/") + Id + TEXT("/default"), Normalize(Var.DefaultValue));
			Snap.Labels.Add(Id, Var.VarName.ToString());
		}

		if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (!Node)
					continue;

				const FString Id = Node->VariableGuid.ToString();
				const USCS_Node* Parent = SCS->FindParentNode(Node);
				const FString ParentName = Parent ? Parent->GetVariableName().ToString() : Node->ParentComponentOrVariableName.ToString();
				Snap.Facts.Add(TEXT("component/") + Id, FString::Printf(TEXT("%s|%s|%s"),
					*Node->GetVariableName().ToString(), *GetPathNameSafe(Node->ComponentClass), *ParentName));
				Snap.Labels.Add(Id, Node->GetVariableName().ToString());

				if (Node->ComponentTemplate)
					AddProperties(Snap, TEXT("component/") + Id + TEXT("/prop/"), Node->ComponentTemplate, Normalize);
			}
		}

		AddBlueprintSettings(Snap, Blueprint, Normalize);
		AddInnerObjects(Snap, Blueprint, Normalize);

		if (Blueprint->GeneratedClass)
		{
			if (const UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false))
				AddProperties(Snap, TEXT("cdo/"), CDO, Normalize);
		}

		struct FNodeLabel
		{
			FString Title;
			FString Graph;
			FString Hint;
		};
		TMap<FString, FNodeLabel> NodeLabels;

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph)
				continue;

			const FString Graph_Id = GraphId(Graph);
			const TCHAR* Kind = Blueprint->FunctionGraphs.Contains(Graph) ? TEXT("function")
				: Blueprint->MacroGraphs.Contains(Graph) ? TEXT("macro")
				: Blueprint->UbergraphPages.Contains(Graph) ? TEXT("event")
				: Blueprint->DelegateSignatureGraphs.Contains(Graph) ? TEXT("delegate")
				: TEXT("sub");
			Snap.Facts.Add(TEXT("graph/") + Graph_Id, Graph->GetName() + TEXT("|") + Kind);
			Snap.Labels.Add(Graph_Id, Graph->GetName());

			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
					continue;

				const FString Id = Node->NodeGuid.ToString();
				const FString Key = TEXT("node/") + Id;
				Snap.Facts.Add(Key, Node->GetClass()->GetPathName() + TEXT("|") + Graph_Id);
				Snap.Facts.Add(Key + TEXT("/title"), Normalize(Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()));
				Snap.Facts.Add(Key + TEXT("/pos"), FString::Printf(TEXT("%d,%d"), Node->NodePosX, Node->NodePosY));
				Snap.Facts.Add(Key + TEXT("/comment"), Node->NodeComment);
				Snap.Facts.Add(Key + TEXT("/enabled"), FString::FromInt(static_cast<int32>(Node->GetDesiredEnabledState())));
				AddNodeSettings(Snap, Key, Node, Normalize);
				// First line of the full title reads best ("Print String", not "PrintString").
				const FString FullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				FString Title = FullTitle;
				FullTitle.Split(TEXT("\n"), &Title, nullptr);
				FNodeLabel& Label = NodeLabels.Add(Id);
				Label.Title = Title;
				Label.Graph = Graph->GetName();

				// Which member variable a Get/Set node uses (by guid; by name for older references).
				if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
				{
					const FMemberReference& Reference = VariableNode->VariableReference;
					if (Reference.IsSelfContext() && !Reference.IsLocalScope())
					{
						FGuid VarGuid = Reference.GetMemberGuid();
						if (!VarGuid.IsValid())
						{
							for (const FBPVariableDescription& Var : Blueprint->NewVariables)
							{
								if (Var.VarName == Reference.GetMemberName())
									VarGuid = Var.VarGuid;
							}
						}
						if (VarGuid.IsValid())
							Snap.NodeVariables.Add(Id, VarGuid.ToString());
					}
				}

				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin)
						continue;

					// A visible input value tells apart nodes with the same title.
					if (Label.Hint.IsEmpty() && Pin->Direction == EGPD_Input && !Pin->bHidden && Pin->LinkedTo.Num() == 0 && !Pin->DefaultValue.IsEmpty() &&
						Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
						Label.Hint = Pin->DefaultValue.Left(40);

					const FString Ref = PinRef(Pin);
					Snap.Facts.Add(TEXT("pin/") + Ref, Normalize(PinDefault(Pin)));

					// The name the editor shows ("In String" for InString); exec pins show none.
					const FString DisplayName = Pin->GetDisplayName().ToString();
					if (!DisplayName.IsEmpty())
						Snap.Labels.Add(Ref, DisplayName);

					const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
					const bool bOutput = Pin->Direction == EGPD_Output;
					if (bExec == bOutput)
						Snap.SingleLinkPins.Add(Ref);
					if (!bOutput && Pin->LinkedTo.Num() > 0)
						Snap.LinkedInputPins.Add(Ref);

					if (!bOutput)
						continue;

					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (Linked && Linked->GetOwningNodeUnchecked())
							Snap.Facts.Add(TEXT("link/") + Ref + TEXT("|") + PinRef(Linked), TEXT("1"));
					}
				}
			}
		}

		// 'Print String' (EventGraph); with a telling input value when the title alone is ambiguous:
		// 'Print String' "Above ground" (EventGraph).
		TMap<FString, int32> NameCounts;
		for (const TPair<FString, FNodeLabel>& Label : NodeLabels)
			++NameCounts.FindOrAdd(Label.Value.Graph + TEXT("/") + Label.Value.Title);

		for (const TPair<FString, FNodeLabel>& Label : NodeLabels)
		{
			const FNodeLabel& Node = Label.Value;
			const bool bAmbiguous = NameCounts[Node.Graph + TEXT("/") + Node.Title] > 1 && !Node.Hint.IsEmpty();
			Snap.Labels.Add(Label.Key, bAmbiguous
				? FString::Printf(TEXT("'%s' \"%s\" (%s)"), *Node.Title, *Node.Hint, *Node.Graph)
				: FString::Printf(TEXT("'%s' (%s)"), *Node.Title, *Node.Graph));
		}

		return Snap;
	}

	FMergePlan PlanMerge(const FSnapshot& Base, const FSnapshot& Ours, const FSnapshot& Theirs)
	{
		TSet<FString> KeySet;
		for (const FSnapshot* Snap : {&Base, &Ours, &Theirs})
		{
			for (const TPair<FString, FString>& Fact : Snap->Facts)
				KeySet.Add(Fact.Key);
		}

		TArray<FString> Keys = KeySet.Array();
		Keys.Sort();

		FMergePlan Plan;
		TMap<FString, int32> IndexOf;

		// 1. Plain three-way rule per fact.
		for (const FString& Key : Keys)
		{
			FDecision& Decision = Plan.Decisions.AddDefaulted_GetRef();
			Decision.Key = Key;
			Decision.Base = FindFact(Base, Key);
			Decision.Ours = FindFact(Ours, Key);
			Decision.Theirs = FindFact(Theirs, Key);

			// The typed-in value of a connected input isn't used (connecting one may even reset it), so it
			// doesn't count as a change on the side that connected it.
			TOptional<FString> OursValue = Decision.Ours;
			TOptional<FString> TheirsValue = Decision.Theirs;
			if (Key.StartsWith(TEXT("pin/")) && Decision.Base.IsSet())
			{
				const FString Ref = Key.Mid(4);
				if (Ours.LinkedInputPins.Contains(Ref))
					OursValue = Decision.Base;
				if (Theirs.LinkedInputPins.Contains(Ref))
					TheirsValue = Decision.Base;
			}

			if (OursValue == TheirsValue)
				Decision.Resolution = OursValue == Decision.Base ? EResolution::Unchanged : EResolution::Both;
			else if (OursValue == Decision.Base)
				Decision.Resolution = EResolution::Theirs;
			else if (TheirsValue == Decision.Base)
				Decision.Resolution = EResolution::Ours;
			else if (Key.EndsWith(TEXT("/pos")) && Decision.Ours.IsSet() && Decision.Theirs.IsSet())
			{
				Decision.Resolution = EResolution::Ours;
				Decision.Reason = TEXT("Moved on both sides; keeping our position.");
			}
			else
			{
				Decision.Resolution = EResolution::Conflict;
				Decision.Reason = TEXT("Changed differently on both sides.");
			}

			IndexOf.Add(Key, Plan.Decisions.Num() - 1);
		}

		// Conflicts that must be decided together end up in one set.
		TArray<int32> Parent;
		Parent.SetNumUninitialized(Plan.Decisions.Num());
		for (int32 Index = 0; Index < Parent.Num(); ++Index)
			Parent[Index] = Index;

		auto FindSet = [&Parent](int32 Index)
		{
			while (Parent[Index] != Index)
				Index = Parent[Index] = Parent[Parent[Index]];
			return Index;
		};

		auto Join = [&Parent, &FindSet](int32 A, int32 B)
		{
			Parent[FindSet(A)] = FindSet(B);
		};

		TMap<FString, TArray<int32>> Members;
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			for (const FString& Owner : OwnersOf(Plan.Decisions[Index].Key))
				Members.FindOrAdd(Owner).Add(Index);
		}

		// 2. One side removed a node/variable/component that the other side still changed or connected to.
		for (const TPair<FString, TArray<int32>>& Group : Members)
		{
			const int32* EntityIndex = IndexOf.Find(Group.Key);
			if (!EntityIndex)
				continue;

			FDecision& Entity = Plan.Decisions[*EntityIndex];
			const bool bRemovedByOurs = Entity.Base.IsSet() && !Entity.Ours.IsSet();
			const bool bRemovedByTheirs = Entity.Base.IsSet() && !Entity.Theirs.IsSet();
			if (bRemovedByOurs == bRemovedByTheirs)
				continue;

			const EResolution RemovingSide = bRemovedByOurs ? EResolution::Ours : EResolution::Theirs;
			const EResolution OtherSide = bRemovedByOurs ? EResolution::Theirs : EResolution::Ours;
			bool bOtherSideChangedIt = Entity.Resolution == EResolution::Conflict;
			for (int32 Member : Group.Value)
			{
				const EResolution Resolution = Plan.Decisions[Member].Resolution;
				bOtherSideChangedIt |= Resolution == OtherSide || Resolution == EResolution::Conflict;
			}

			if (!bOtherSideChangedIt)
				continue;

			const TCHAR* Reason = bRemovedByOurs
				? TEXT("Removed in ours but changed in theirs.")
				: TEXT("Removed in theirs but changed in ours.");
			Entity.Resolution = EResolution::Conflict;
			Entity.Reason = Reason;
			// Everything that went away with the removal is part of the decision too: keeping the
			// other side's version must bring the node back with its pins and links.
			for (int32 Member : Group.Value)
			{
				FDecision& Decision = Plan.Decisions[Member];
				if (Decision.Resolution == OtherSide || Decision.Resolution == RemovingSide)
				{
					Decision.Resolution = EResolution::Conflict;
					Decision.Reason = Reason;
				}
				if (Decision.Resolution == EResolution::Conflict)
					Join(Member, *EntityIndex);
			}
		}

		auto MarkConflict = [&Plan, &Join](int32 Index, const FString& Reason, int32 Anchor)
		{
			FDecision& Decision = Plan.Decisions[Index];
			if (Decision.Resolution == EResolution::Unchanged)
				return;
			if (Decision.Resolution != EResolution::Conflict)
			{
				Decision.Resolution = EResolution::Conflict;
				Decision.Reason = Reason;
			}
			Join(Index, Anchor);
		};

		// A variable with everything that goes with it: its details and its class default.
		auto AddVariable = [&Plan, &Members, &IndexOf](int32 Index, const TOptional<FString>& Value, TArray<int32>& Out)
		{
			Out.Add(Index);
			if (const TArray<int32>* VariableMembers = Members.Find(Plan.Decisions[Index].Key))
				Out.Append(*VariableMembers);
			if (const int32* Default = IndexOf.Find(TEXT("cdo/") + VariableField(Value, 0)))
				Out.Add(*Default);
		};

		// 2b. One side removed a member variable that the other side's new or changed Get/Set nodes still use.
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			const FDecision Variable = Plan.Decisions[Index];
			if (!Variable.Key.StartsWith(TEXT("var/")) || !IsEntityKey(Variable.Key) || !Variable.Base.IsSet() || Variable.Ours.IsSet() == Variable.Theirs.IsSet())
				continue;

			const bool bRemovedByOurs = !Variable.Ours.IsSet();
			const FSnapshot& Keeping = bRemovedByOurs ? Theirs : Ours;
			const EResolution KeepingSide = bRemovedByOurs ? EResolution::Theirs : EResolution::Ours;
			const FString VariableId = Variable.Key.Mid(4);

			TArray<int32> Involved;
			for (const TPair<FString, FString>& Use : Keeping.NodeVariables)
			{
				const int32* NodeIndex = Use.Value == VariableId ? IndexOf.Find(TEXT("node/") + Use.Key) : nullptr;
				if (!NodeIndex)
					continue;

				const EResolution NodeResolution = Plan.Decisions[*NodeIndex].Resolution;
				if (NodeResolution != KeepingSide && NodeResolution != EResolution::Conflict)
					continue;

				Involved.Add(*NodeIndex);
				if (const TArray<int32>* NodeMembers = Members.Find(Plan.Decisions[*NodeIndex].Key))
					Involved.Append(*NodeMembers);
			}

			if (Involved.IsEmpty())
				continue;

			AddVariable(Index, Variable.Base, Involved);
			const FString Reason = bRemovedByOurs ? TEXT("Removed in ours but used in theirs.") : TEXT("Removed in theirs but used in ours.");
			for (int32 Member : Involved)
				MarkConflict(Member, Reason, Index);
		}

		// 2c. Both sides added a member variable with the same name; only one of them can stay.
		TMap<FString, int32> AddedByOurs;
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			const FDecision& Decision = Plan.Decisions[Index];
			if (Decision.Key.StartsWith(TEXT("var/")) && IsEntityKey(Decision.Key) && !Decision.Base.IsSet() && Decision.Ours.IsSet() && !Decision.Theirs.IsSet())
				AddedByOurs.Add(VariableField(Decision.Ours, 0), Index);
		}

		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			const FDecision Decision = Plan.Decisions[Index];
			if (!Decision.Key.StartsWith(TEXT("var/")) || !IsEntityKey(Decision.Key) || Decision.Base.IsSet() || Decision.Ours.IsSet() || !Decision.Theirs.IsSet())
				continue;

			const FString Name = VariableField(Decision.Theirs, 0);
			const int32* Mine = AddedByOurs.Find(Name);
			if (!Mine)
				continue;

			TArray<int32> Involved;
			AddVariable(*Mine, Plan.Decisions[*Mine].Ours, Involved);
			AddVariable(Index, Decision.Theirs, Involved);
			const FString Reason = FString::Printf(TEXT("Both sides added a variable named '%s'."), *Name);
			for (int32 Member : Involved)
				MarkConflict(Member, Reason, *Mine);
		}

		// 2d. One side removed a graph (e.g. a function) whose nodes the other side still changed or added to:
		// keeping it must keep all of it, so the whole graph is one decision.
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			const FDecision Graph = Plan.Decisions[Index];
			if (!Graph.Key.StartsWith(TEXT("graph/")) || !Graph.Base.IsSet() || Graph.Ours.IsSet() == Graph.Theirs.IsSet())
				continue;

			const bool bRemovedByOurs = !Graph.Ours.IsSet();
			const EResolution KeepingSide = bRemovedByOurs ? EResolution::Theirs : EResolution::Ours;
			const FString ThisGraph = Graph.Key.Mid(6);

			TArray<int32> Nodes;
			bool bChangedByKeepingSide = false;
			for (int32 NodeIndex = 0; NodeIndex < Plan.Decisions.Num(); ++NodeIndex)
			{
				const FDecision& Node = Plan.Decisions[NodeIndex];
				if (!Node.Key.StartsWith(TEXT("node/")) || !IsEntityKey(Node.Key))
					continue;

				// "<class>|<graph>"
				const TOptional<FString>& Value = Node.Base.IsSet() ? Node.Base : (bRemovedByOurs ? Node.Theirs : Node.Ours);
				FString Class, InGraph;
				if (!Value.IsSet() || !Value->Split(TEXT("|"), &Class, &InGraph) || InGraph != ThisGraph)
					continue;

				Nodes.Add(NodeIndex);
				bChangedByKeepingSide |= Node.Resolution == KeepingSide || Node.Resolution == EResolution::Conflict;
				if (const TArray<int32>* NodeMembers = Members.Find(Node.Key))
				{
					for (int32 Member : *NodeMembers)
						bChangedByKeepingSide |= Plan.Decisions[Member].Resolution == KeepingSide;
				}
			}

			if (!bChangedByKeepingSide)
				continue;

			TArray<int32> Involved = {Index};
			for (int32 NodeIndex : Nodes)
			{
				Involved.Add(NodeIndex);
				if (const TArray<int32>* NodeMembers = Members.Find(Plan.Decisions[NodeIndex].Key))
					Involved.Append(*NodeMembers);
			}

			const FString Reason = bRemovedByOurs ? TEXT("Removed in ours but changed in theirs.") : TEXT("Removed in theirs but changed in ours.");
			for (int32 Member : Involved)
				MarkConflict(Member, Reason, Index);
		}

		// 3. A pin that holds one link (exec output, data input) ended up with links from both sides.
		TSet<FString> SingleLinkPins = Base.SingleLinkPins;
		SingleLinkPins.Append(Ours.SingleLinkPins);
		SingleLinkPins.Append(Theirs.SingleLinkPins);

		TMap<FString, TArray<int32>> LinksAtPin;
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			const FDecision& Decision = Plan.Decisions[Index];
			FString Src, Dst;
			if (Decision.Resolution == EResolution::Conflict || !SplitLink(Decision.Key, Src, Dst) || !MergedValue(Decision).IsSet())
				continue;

			if (SingleLinkPins.Contains(Src))
				LinksAtPin.FindOrAdd(Src).Add(Index);
			if (SingleLinkPins.Contains(Dst))
				LinksAtPin.FindOrAdd(Dst).Add(Index);
		}

		for (const TPair<FString, TArray<int32>>& Pin : LinksAtPin)
		{
			if (Pin.Value.Num() < 2)
				continue;

			int32 First = INDEX_NONE;
			for (int32 Index : Pin.Value)
			{
				FDecision& Decision = Plan.Decisions[Index];
				if (Decision.Resolution == EResolution::Ours || Decision.Resolution == EResolution::Theirs)
				{
					Decision.Resolution = EResolution::Conflict;
					Decision.Reason = TEXT("This pin takes a single connection and both sides connected it differently.");
				}

				if (Decision.Resolution != EResolution::Conflict)
					continue;

				if (First == INDEX_NONE)
					First = Index;
				else
					Join(Index, First);
			}
		}

		// 3b. Nodes a side added only to sit in a contested connection (e.g. a Delay put after an exec pin) go
		// with that side's choice; otherwise the losing side leaves them behind, unreachable. A node that is
		// also called from somewhere this conflict doesn't decide stays out of it.
		auto AddedBy = [&Plan, &IndexOf](const FString& NodeId, bool bOurs) -> int32
		{
			const int32* Index = IndexOf.Find(TEXT("node/") + NodeId);
			if (!Index)
				return INDEX_NONE;
			const FDecision& Node = Plan.Decisions[*Index];
			const TOptional<FString>& Mine = bOurs ? Node.Ours : Node.Theirs;
			const TOptional<FString>& Other = bOurs ? Node.Theirs : Node.Ours;
			return !Node.Base.IsSet() && Mine.IsSet() && !Other.IsSet() ? *Index : INDEX_NONE;
		};

		auto LinkNodes = [](const FString& Key, FString& SrcNode, FString& DstNode)
		{
			FString Src, Dst, Pin;
			if (!SplitLink(Key, Src, Dst))
				return false;
			SplitPinRef(Src, SrcNode, Pin);
			SplitPinRef(Dst, DstNode, Pin);
			return true;
		};

		TMap<FString, TArray<int32>> LinksInto, LinksFrom;
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			FString SrcNode, DstNode;
			if (LinkNodes(Plan.Decisions[Index].Key, SrcNode, DstNode))
			{
				LinksFrom.FindOrAdd(SrcNode).Add(Index);
				LinksInto.FindOrAdd(DstNode).Add(Index);
			}
		}

		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			const FDecision Link = Plan.Decisions[Index];
			FString SrcNode, DstNode;
			if (Link.Resolution != EResolution::Conflict || Link.Base.IsSet() || Link.Ours.IsSet() == Link.Theirs.IsSet() || !LinkNodes(Link.Key, SrcNode, DstNode))
				continue;

			const bool bOurs = Link.Ours.IsSet();
			TArray<FString> Queue = {SrcNode, DstNode};
			TSet<FString> Pulled;
			while (Queue.Num() > 0)
			{
				const FString NodeId = Queue.Pop();
				const int32 NodeIndex = AddedBy(NodeId, bOurs);
				if (NodeIndex == INDEX_NONE || Pulled.Contains(NodeId))
					continue;

				bool bUsedElsewhere = false;
				for (int32 In : LinksInto.FindRef(NodeId))
				{
					const FDecision& Incoming = Plan.Decisions[In];
					FString InSrc, InDst;
					LinkNodes(Incoming.Key, InSrc, InDst);
					const bool bOnThisSide = (bOurs ? Incoming.Ours : Incoming.Theirs).IsSet();
					bUsedElsewhere |= bOnThisSide && Incoming.Resolution != EResolution::Conflict && AddedBy(InSrc, bOurs) == INDEX_NONE;
				}
				if (bUsedElsewhere)
					continue;

				Pulled.Add(NodeId);
				MarkConflict(NodeIndex, Link.Reason, Index);
				for (int32 Member : Members.FindRef(Plan.Decisions[NodeIndex].Key))
					MarkConflict(Member, Link.Reason, Index);

				// The rest of what this side put in: nodes it added that are wired to this one.
				FString OtherSrc, OtherDst;
				for (int32 Out : LinksFrom.FindRef(NodeId))
				{
					if (LinkNodes(Plan.Decisions[Out].Key, OtherSrc, OtherDst))
						Queue.Add(OtherDst);
				}
				for (int32 In : LinksInto.FindRef(NodeId))
				{
					if (LinkNodes(Plan.Decisions[In].Key, OtherSrc, OtherDst))
						Queue.Add(OtherSrc);
				}
			}
		}

		// Number the groups in key order so reports and choices files stay stable.
		TMap<int32, int32> GroupOfSet;
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			FDecision& Decision = Plan.Decisions[Index];
			if (Decision.Resolution != EResolution::Conflict)
				continue;

			const int32 Set = FindSet(Index);
			if (const int32* Existing = GroupOfSet.Find(Set))
			{
				Decision.Group = *Existing;
			}
			else
			{
				Decision.Group = Plan.NumGroups++;
				GroupOfSet.Add(Set, Decision.Group);
			}
		}

		FinalizePlan(Plan);
		return Plan;
	}

	void FinalizePlan(FMergePlan& Plan)
	{
		TMap<FString, int32> IndexOf;
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
			IndexOf.Add(Plan.Decisions[Index].Key, Index);

		// Hide per-node details of nodes that were added/removed as a whole.
		for (FDecision& Decision : Plan.Decisions)
		{
			Decision.bPartOfWhole = false;
			if (Decision.Resolution == EResolution::Unchanged || Decision.Resolution == EResolution::Conflict || Decision.Key.StartsWith(TEXT("link/")))
				continue;

			for (const FString& Owner : OwnersOf(Decision.Key))
			{
				const int32* OwnerIndex = IndexOf.Find(Owner);
				if (!OwnerIndex)
					continue;

				// "Whole" = the entity appears or disappears in the result: added, removed, or restored
				// from theirs after ours removed it.
				const FDecision& Entity = Plan.Decisions[*OwnerIndex];
				const bool bWhole = Entity.Resolution == EResolution::Theirs
					? Entity.Ours.IsSet() != Entity.Theirs.IsSet()
					: Entity.Base.IsSet() != MergedValue(Entity).IsSet();
				if (bWhole && Entity.Resolution == Decision.Resolution)
					Decision.bPartOfWhole = true;
			}
		}

		Plan.NumOurs = Plan.NumTheirs = Plan.NumBoth = Plan.NumConflicts = 0;
		TSet<int32> OpenGroups;
		for (const FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Resolution == EResolution::Conflict)
			{
				OpenGroups.Add(Decision.Group);
				continue;
			}

			if (Decision.bPartOfWhole)
				continue;

			switch (Decision.Resolution)
			{
			case EResolution::Ours: ++Plan.NumOurs; break;
			case EResolution::Theirs: ++Plan.NumTheirs; break;
			case EResolution::Both: ++Plan.NumBoth; break;
			default: break;
			}
		}

		// Conflicts are counted per group: that's how many decisions a person has to make.
		Plan.NumConflicts = OpenGroups.Num();
	}

	bool ResolveConflicts(FMergePlan& Plan, const TMap<int32, EResolution>& Choices)
	{
		for (const FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Resolution != EResolution::Conflict)
				continue;

			const EResolution* Side = Choices.Find(Decision.Group);
			if (!Side || (*Side != EResolution::Ours && *Side != EResolution::Theirs))
				return false;
		}

		// Taking one side for a whole group keeps it consistent: e.g. both links of a single-link pin
		// take that side's value, so exactly one of them survives.
		for (FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Resolution != EResolution::Conflict)
				continue;

			Decision.Resolution = Choices[Decision.Group];
			Decision.Reason = Decision.Resolution == EResolution::Ours ? TEXT("Conflict resolved: kept ours.") : TEXT("Conflict resolved: took theirs.");
		}

		FinalizePlan(Plan);
		return true;
	}

	bool FindInsertions(const FMergePlan& Plan, int32 Group, const FSnapshot& Ours, const FSnapshot& Theirs, FInsertion& OutOurs, FInsertion& OutTheirs)
	{
		TArray<int32> Members;
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			if (Plan.Decisions[Index].Group == Group)
				Members.Add(Index);
		}

		auto OnlyOn = [](const FDecision& Decision, bool bOurs)
		{
			return !Decision.Base.IsSet() && (bOurs ? Decision.Ours : Decision.Theirs).IsSet() && !(bOurs ? Decision.Theirs : Decision.Ours).IsSet();
		};

		// The exec output both sides connected differently.
		FString Contested;
		{
			TMap<FString, int32> SidesAtSrc;
			for (int32 Member : Members)
			{
				const FDecision& Decision = Plan.Decisions[Member];
				FString Src, Dst;
				if (SplitLink(Decision.Key, Src, Dst) && Src.Contains(TEXT(":out:")) && (Ours.SingleLinkPins.Contains(Src) || Theirs.SingleLinkPins.Contains(Src)))
					SidesAtSrc.FindOrAdd(Src) |= (OnlyOn(Decision, true) ? 1 : 0) | (OnlyOn(Decision, false) ? 2 : 0);
			}
			for (const TPair<FString, int32>& Pin : SidesAtSrc)
			{
				if (Pin.Value == 3)
					Contested = Pin.Key;
			}
		}
		if (Contested.IsEmpty())
			return false;

		auto Find = [&](bool bOurs, FInsertion& Out)
		{
			const FSnapshot& Side = bOurs ? Ours : Theirs;
			for (int32 Member : Members)
			{
				const FDecision& Decision = Plan.Decisions[Member];
				if (Decision.Key.StartsWith(TEXT("node/")) && IsEntityKey(Decision.Key) && OnlyOn(Decision, bOurs))
					Out.Nodes.Add(Decision.Key.Mid(5));
			}

			for (int32 Member : Members)
			{
				const FDecision& Decision = Plan.Decisions[Member];
				FString Src, Dst, SrcNode, DstNode, Pin;
				if (!OnlyOn(Decision, bOurs) || !SplitLink(Decision.Key, Src, Dst))
					continue;
				SplitPinRef(Src, SrcNode, Pin);
				SplitPinRef(Dst, DstNode, Pin);

				if (Src == Contested)
				{
					if (Out.Entry != INDEX_NONE || !Out.Nodes.Contains(DstNode))
						return false;
					Out.Entry = Member;
				}
				else if (Out.Nodes.Contains(SrcNode) && !Out.Nodes.Contains(DstNode) && Side.SingleLinkPins.Contains(Src))
				{
					// An exec link out of the new nodes: where the connection carries on.
					if (Out.Exit != INDEX_NONE)
						return false;
					Out.Exit = Member;
				}
			}
			return Out.Entry != INDEX_NONE && Out.Exit != INDEX_NONE;
		};

		if (!Find(true, OutOurs) || !Find(false, OutTheirs))
			return false;

		// Both lead back to the same place, so one can follow the other.
		FString Src, OursDst, TheirsDst;
		SplitLink(Plan.Decisions[OutOurs.Exit].Key, Src, OursDst);
		SplitLink(Plan.Decisions[OutTheirs.Exit].Key, Src, TheirsDst);
		return OursDst == TheirsDst;
	}

	TArray<FString> TakeBoth(FMergePlan& Plan, int32 Group, bool bOursFirst, const FSnapshot& Ours, const FSnapshot& Theirs)
	{
		FInsertion OursPart, TheirsPart;
		if (!FindInsertions(Plan, Group, Ours, Theirs, OursPart, TheirsPart))
			return {};

		const FInsertion& First = bOursFirst ? OursPart : TheirsPart;
		const FInsertion& Second = bOursFirst ? TheirsPart : OursPart;

		// Everything either side did here is kept...
		for (FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Group != Group)
				continue;
			Decision.Resolution = Decision.Ours.IsSet() || !Decision.Theirs.IsSet() ? EResolution::Ours : EResolution::Theirs;
			Decision.Reason = TEXT("Conflict resolved: took both.");
		}

		// ...except the second part's way in and the first part's way out, which become one link between them.
		for (int32 Dropped : {Second.Entry, First.Exit})
		{
			FDecision& Decision = Plan.Decisions[Dropped];
			Decision.Resolution = Decision.Ours.IsSet() ? EResolution::Theirs : EResolution::Ours;
		}

		FString FirstOut, SecondIn, Unused;
		SplitLink(Plan.Decisions[First.Exit].Key, FirstOut, Unused);
		SplitLink(Plan.Decisions[Second.Entry].Key, Unused, SecondIn);

		FDecision& Joint = Plan.Decisions.AddDefaulted_GetRef();
		Joint.Key = TEXT("link/") + FirstOut + TEXT("|") + SecondIn;
		Joint.Theirs = FString(TEXT("1"));
		Joint.Resolution = EResolution::Theirs;
		Joint.Reason = TEXT("Joins both sides' nodes.");

		return Second.Nodes;
	}

	int32 GroupHead(const FMergePlan& Plan, int32 Group)
	{
		int32 First = INDEX_NONE;
		int32 FirstEntity = INDEX_NONE;
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			const FDecision& Decision = Plan.Decisions[Index];
			if (Decision.Group != Group)
				continue;

			if (First == INDEX_NONE)
				First = Index;

			// A removed graph is what the whole group is about.
			if (Decision.Key.StartsWith(TEXT("graph/")) && Decision.Base.IsSet())
				return Index;

			if (!IsEntityKey(Decision.Key))
				continue;

			// What one side removed names the group better than what the other side added for it.
			if (Decision.Base.IsSet())
				return Index;
			if (FirstEntity == INDEX_NONE)
				FirstEntity = Index;
		}
		return FirstEntity != INDEX_NONE ? FirstEntity : First;
	}

	bool IsWorthShowing(const FDecision& Decision)
	{
		FString Src, Dst;
		if (IsEntityKey(Decision.Key) || SplitLink(Decision.Key, Src, Dst) || Decision.Resolution != EResolution::Conflict)
			return true;

		// Details of a node/variable one side added, or values the side that kept it never touched.
		return Decision.Base.IsSet()
			&& ((Decision.Ours.IsSet() && Decision.Ours != Decision.Base) || (Decision.Theirs.IsSet() && Decision.Theirs != Decision.Base));
	}

	bool IsUnsupported(const FDecision& Decision, const FSnapshot& Ours, const FSnapshot& Theirs)
	{
		const FString& Key = Decision.Key;
		if (Key == TEXT("parent") || Key.StartsWith(TEXT("interface/")) || Ours.Unmergeable.Contains(Key) || Theirs.Unmergeable.Contains(Key))
			return true;

		// Widgets theirs added go into a panel ours has (or one theirs adds along with them). Removing, replacing or
		// restoring one is not supported yet.
		if (Key.StartsWith(TEXT("inner/")) && IsEntityKey(Key))
		{
			if (Decision.Base.IsSet() || Decision.Ours.IsSet() || !Decision.Theirs.IsSet())
				return true;

			FString Current = Key;
			for (int32 Depth = 0; Depth < 100; ++Depth)
			{
				const FString* Parent = Theirs.WidgetParents.Find(Current);
				if (!Parent || Parent->IsEmpty())
					return true;
				if (Ours.Facts.Contains(*Parent))
					return false;
				Current = *Parent;
			}
			return true;
		}

		// Function graphs can be added, removed and renamed; other kinds of graphs not yet.
		if (Key.StartsWith(TEXT("graph/")))
			return VariableField(Decision.Theirs.IsSet() ? Decision.Theirs : Decision.Base, 1) != TEXT("function");

		// Settings of a component both versions have can be merged; adding, removing or moving one not yet.
		if (Key.StartsWith(TEXT("component/")))
		{
			FString Id = Key.Mid(10), Detail;
			Key.Mid(10).Split(TEXT("/"), &Id, &Detail);
			return !Detail.StartsWith(TEXT("prop/")) || !Ours.Facts.Contains(TEXT("component/") + Id) || !Theirs.Facts.Contains(TEXT("component/") + Id);
		}

		if (!Key.StartsWith(TEXT("node/")) || !IsEntityKey(Key) || !Decision.Theirs.IsSet())
			return false;

		// "<class>|<graph>": a node in a graph ours doesn't have (fine when that graph is a function theirs
		// added), or one theirs moved to another graph.
		FString Class, Graph;
		Decision.Theirs->Split(TEXT("|"), &Class, &Graph);
		if (!Ours.Facts.Contains(TEXT("graph/") + Graph))
			return VariableField(FindFact(Theirs, TEXT("graph/") + Graph), 1) != TEXT("function");
		return Decision.Base.IsSet() && Decision.Ours.IsSet();
	}

	TArray<int32> FindUnsupported(const FMergePlan& Plan, const FSnapshot& Ours, const FSnapshot& Theirs)
	{
		TSet<FString> NewGraphs;
		for (const FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Key.StartsWith(TEXT("graph/")) && !Decision.Ours.IsSet() && Decision.Theirs.IsSet())
				NewGraphs.Add(Decision.Key.Mid(6));
		}

		TArray<int32> Found;
		for (int32 Index = 0; Index < Plan.Decisions.Num(); ++Index)
		{
			const FDecision& Decision = Plan.Decisions[Index];
			const bool bTaken = (Decision.Resolution == EResolution::Theirs && !Decision.bPartOfWhole) || Decision.Resolution == EResolution::Conflict;
			if (!bTaken || !IsUnsupported(Decision, Ours, Theirs))
				continue;

			// Nodes of a graph only theirs has are covered by that graph.
			FString Class, Graph;
			if (Decision.Key.StartsWith(TEXT("node/")) && Decision.Theirs->Split(TEXT("|"), &Class, &Graph) && NewGraphs.Contains(Graph))
				continue;

			// So are the details of a component that is listed itself.
			const TArray<FString, TInlineAllocator<2>> Owners = OwnersOf(Decision.Key);
			if (Owners.Num() == 1 && Found.ContainsByPredicate([&Plan, &Owners](int32 Other) { return Plan.Decisions[Other].Key == Owners[0]; }))
				continue;

			Found.Add(Index);
		}
		return Found;
	}

	void LeaveOutUnsupported(FMergePlan& Plan, const FSnapshot& Ours, const FSnapshot& Theirs)
	{
		TSet<FString> LeftOutEntities;
		for (FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Resolution != EResolution::Theirs || !IsUnsupported(Decision, Ours, Theirs))
				continue;

			Decision.Resolution = EResolution::Ours;
			Decision.Reason = TEXT("Left out: not supported yet.");
			if (IsEntityKey(Decision.Key))
				LeftOutEntities.Add(Decision.Key);
		}

		// Details and links of what was left out stay as in ours too.
		for (FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Resolution != EResolution::Theirs)
				continue;

			for (const FString& Owner : OwnersOf(Decision.Key))
			{
				if (LeftOutEntities.Contains(Owner))
				{
					Decision.Resolution = EResolution::Ours;
					Decision.Reason = TEXT("Left out: not supported yet.");
					break;
				}
			}
		}

		FinalizePlan(Plan);
	}

	FString NodeIdOfKey(const FString& Key)
	{
		FString Src, Dst, Node, Pin;
		if (SplitLink(Key, Src, Dst))
		{
			SplitPinRef(Src, Node, Pin);
			return Node;
		}

		if (Key.StartsWith(TEXT("pin/")))
		{
			SplitPinRef(Key.Mid(4), Node, Pin);
			return Node;
		}

		if (Key.StartsWith(TEXT("node/")))
		{
			const FString Rest = Key.Mid(5);
			return Rest.Split(TEXT("/"), &Node, &Pin) ? Node : Rest;
		}

		return FString();
	}

	namespace
	{
		// " -> " drawn as a right arrow (U+2192); built from its code so the source stays ASCII.
		const FString Arrow = FString::Printf(TEXT(" %c "), static_cast<TCHAR>(0x2192));

		/** "<prefix><id>/<detail>" -> id, detail (empty for the entity itself). */
		void SplitEntity(const FString& Key, FString& Id, FString& Detail)
		{
			const FString Rest = Key.Mid(Key.Find(TEXT("/")) + 1);
			if (!Rest.Split(TEXT("/"), &Id, &Detail))
			{
				Id = Rest;
				Detail.Reset();
			}
		}

		/** Readable names for what a fact key points at, looked up in the three snapshots. */
		struct FNamer
		{
			const FSnapshot& Base;
			const FSnapshot& Ours;
			const FSnapshot& Theirs;

			// Base first: a node keeps the same name in every line even when the sides changed its values.
			FString Label(const FString& Id) const
			{
				for (const FSnapshot* Snap : {&Base, &Ours, &Theirs})
				{
					if (const FString* Found = Snap->Labels.Find(Id))
						return *Found;
				}
				return Id;
			}

			/** "<node>:<in|out>:<pin>" -> "In String" (display name), or the raw pin name for exec pins. */
			FString PinName(const FString& Ref) const
			{
				const FString Display = Label(Ref);
				if (Display != Ref)
					return Display;

				FString Node, Key, Name;
				SplitPinRef(Ref, Node, Key);
				return Key.Split(TEXT(":"), nullptr, &Name) ? Name : Key;
			}

			FString PinAt(const FString& Ref) const
			{
				FString Node, Pin;
				SplitPinRef(Ref, Node, Pin);
				return Label(Node) + TEXT(".") + PinName(Ref);
			}

			FString Value(const FString& Key, const TOptional<FString>& InValue) const
			{
				if (!InValue.IsSet())
					return TEXT("(removed)");

				if (Key.EndsWith(TEXT("/enabled")))
				{
					static const TCHAR* States[] = {TEXT("Enabled"), TEXT("Disabled"), TEXT("Development Only")};
					return States[FMath::Clamp(FCString::Atoi(*InValue.GetValue()), 0, 2)];
				}

				if (InValue->IsEmpty())
					return TEXT("(empty)");

				// "<name>|<kind>": the name is what people know a graph by.
				if (Key.StartsWith(TEXT("graph/")))
					return TEXT("\"") + VariableField(InValue, 0) + TEXT("\"");

				return TEXT("\"") + TrimZeros(Shorten(ShortObjectNames(ReadableText(*InValue))).Mid(1).LeftChop(1)) + TEXT("\"");
			}

			/** The thing a fact is about, e.g. "In String of 'Print String' (EventGraph)". */
			FString Subject(const FString& Key) const
			{
				FString Src, Dst, Id, Detail;
				if (SplitLink(Key, Src, Dst))
					return TEXT("Connection ") + PinAt(Src) + Arrow + PinAt(Dst);

				if (Key.StartsWith(TEXT("pin/")))
				{
					FString Node, Pin;
					SplitPinRef(Key.Mid(4), Node, Pin);
					return PinName(Key.Mid(4)) + TEXT(" of ") + Label(Node);
				}

				if (Key.StartsWith(TEXT("node/")))
				{
					SplitEntity(Key, Id, Detail);
					if (Detail.IsEmpty())
						return TEXT("Node ") + Label(Id);
					if (Detail == TEXT("pos"))
						return TEXT("Position of ") + Label(Id);
					if (Detail == TEXT("comment"))
						return TEXT("Comment of ") + Label(Id);
					if (Detail == TEXT("enabled"))
						return TEXT("Enabled state of ") + Label(Id);
					if (Detail.StartsWith(TEXT("prop/")))
						return TEXT("Setting '") + Label(Key) + TEXT("' of ") + Label(Id);
					return TEXT("Setup of ") + Label(Id);
				}

				if (Key.StartsWith(TEXT("var/")))
				{
					SplitEntity(Key, Id, Detail);
					return (Detail.IsEmpty() ? TEXT("Variable '") : TEXT("Default value of variable '")) + Label(Id) + TEXT("'");
				}

				if (Key.StartsWith(TEXT("component/")))
				{
					SplitEntity(Key, Id, Detail);
					FString Property;
					Detail.Split(TEXT("prop/"), nullptr, &Property);
					return Detail.IsEmpty()
						? TEXT("Component '") + Label(Id) + TEXT("'")
						: TEXT("'") + Property + TEXT("' of component '") + Label(Id) + TEXT("'");
				}

				// Data assets, data tables, material instances (AssetMerge.h): readable names are kept per key.
				if (Key.StartsWith(TEXT("prop/")))
					return TEXT("Property '") + Label(Key) + TEXT("'");
				if (Key.StartsWith(TEXT("row/")))
				{
					SplitEntity(Key, Id, Detail);
					return Detail.IsEmpty()
						? TEXT("Row '") + Label(Key) + TEXT("'")
						: TEXT("'") + Label(Key) + TEXT("' of row '") + Label(TEXT("row/") + Id) + TEXT("'");
				}
				if (Key.StartsWith(TEXT("bp/")))
					return TEXT("Class setting '") + Label(Key) + TEXT("'");
				if (Key.StartsWith(TEXT("inner/")))
				{
					SplitEntity(Key, Id, Detail);
					return Detail.IsEmpty() ? Label(Key) : TEXT("'") + Label(Key) + TEXT("' of ") + Label(TEXT("inner/") + Id);
				}
				if (Label(Key) != Key)
					return Label(Key);

				if (Key.StartsWith(TEXT("cdo/")))
					return TEXT("Class default '") + Key.Mid(4) + TEXT("'");
				if (Key.StartsWith(TEXT("interface/")))
					return TEXT("Interface ") + FPackageName::ObjectPathToObjectName(Key.Mid(10));
				if (Key.StartsWith(TEXT("graph/")))
					return TEXT("Graph '") + Label(Key.Mid(6)) + TEXT("'");
				if (Key == TEXT("parent"))
					return TEXT("Parent class");
				return Key;
			}

			/** Short form of one side's edit, for listing what a side did to something the other side removed. */
			FString Edit(const FDecision& Decision, const TOptional<FString>& NewValue) const
			{
				if (IsEntityKey(Decision.Key))
				{
					FString What = Subject(Decision.Key);
					What[0] = FChar::ToLower(What[0]);
					return (!NewValue.IsSet() ? TEXT("removed ") : !Decision.Base.IsSet() ? TEXT("added ") : TEXT("changed ")) + What;
				}

				FString Src, Dst, Id, Detail;
				if (SplitLink(Decision.Key, Src, Dst))
					return (NewValue.IsSet() ? TEXT("connected ") : TEXT("disconnected ")) + PinAt(Src) + Arrow + PinAt(Dst);

				if (Decision.Key.StartsWith(TEXT("pin/")))
					return PinName(Decision.Key.Mid(4)) + TEXT(" = ") + Value(Decision.Key, NewValue);

				if (Decision.Key.StartsWith(TEXT("node/")))
				{
					SplitEntity(Decision.Key, Id, Detail);
					if (Detail == TEXT("pos"))
						return TEXT("moved");
					if (Detail == TEXT("comment"))
						return TEXT("comment ") + Value(Decision.Key, NewValue);
					if (Detail == TEXT("enabled"))
						return TEXT("set to ") + Value(Decision.Key, NewValue);
					if (Detail.StartsWith(TEXT("prop/")))
						return Label(Decision.Key) + TEXT(" = ") + Value(Decision.Key, NewValue);
					return TEXT("setup changed");
				}

				return Subject(Decision.Key) + TEXT(" = ") + Value(Decision.Key, NewValue);
			}
		};
	}

	FString Describe(const FDecision& Decision, const FSnapshot& Base, const FSnapshot& Ours, const FSnapshot& Theirs)
	{
		const FNamer Names{Base, Ours, Theirs};
		const FString& Key = Decision.Key;
		const bool bConflict = Decision.Resolution == EResolution::Conflict;
		const TOptional<FString>& After = MergedValue(Decision);

		FString Src, Dst, Id, Detail;
		if (SplitLink(Key, Src, Dst))
		{
			const FString Link = Names.PinAt(Src) + Arrow + Names.PinAt(Dst);
			if (bConflict)
				return TEXT("Connection ") + Link + (Decision.Ours.IsSet() ? TEXT(": only in ours") : TEXT(": only in theirs"));
			return (Decision.Base.IsSet() ? TEXT("Disconnected ") : TEXT("Connected ")) + Link;
		}

		if (IsEntityKey(Key) || Key.StartsWith(TEXT("interface/")) || Key.StartsWith(TEXT("graph/")))
		{
			if (bConflict)
				return Names.Subject(Key) + TEXT(": ") + Decision.Reason;

			const TCHAR* Verb = !Decision.Base.IsSet() ? TEXT("Added ") : !After.IsSet() ? TEXT("Removed ") : TEXT("Changed ");
			FString Subject = Names.Subject(Key);
			Subject[0] = FChar::ToLower(Subject[0]);
			return Verb + Subject;
		}

		if (Key.StartsWith(TEXT("node/")))
		{
			SplitEntity(Key, Id, Detail);
			if (Detail == TEXT("pos") && !bConflict)
				return TEXT("Moved ") + Names.Label(Id);
			if (Detail == TEXT("enabled") && !bConflict)
				return TEXT("Set ") + Names.Label(Id) + TEXT(" to ") + Names.Value(Key, After);
		}

		if (bConflict)
			return Names.Subject(Key) + TEXT(": ours ") + Names.Value(Key, Decision.Ours) + TEXT(", theirs ") + Names.Value(Key, Decision.Theirs);

		return Names.Subject(Key) + TEXT(": ") + Names.Value(Key, Decision.Base) + Arrow + Names.Value(Key, After);
	}

	bool IsValueKey(const FString& Key)
	{
		return !Key.StartsWith(TEXT("node/")) && !Key.StartsWith(TEXT("pin/")) && !Key.StartsWith(TEXT("link/"));
	}

	FString DescribeSubject(const FString& Key, const FSnapshot& Base, const FSnapshot& Ours, const FSnapshot& Theirs)
	{
		return FNamer{Base, Ours, Theirs}.Subject(Key);
	}

	FString DescribeValue(const FString& Key, const TOptional<FString>& Value, const FSnapshot& Base, const FSnapshot& Ours, const FSnapshot& Theirs)
	{
		if (!Value.IsSet())
			return TEXT("Not in this version");

		if (Key.StartsWith(TEXT("var/")) && IsEntityKey(Key))
		{
			const FString Category = VariableField(Value, 2);
			return VariableField(Value, 1) + (Category.IsEmpty() || Category == TEXT("Default") ? FString() : TEXT(", category ") + Category);
		}
		if (Key.StartsWith(TEXT("component/")) && IsEntityKey(Key))
			return FPackageName::ObjectPathToObjectName(VariableField(Value, 1));
		if (Key.StartsWith(TEXT("graph/")) || Key.StartsWith(TEXT("interface/")) || ((Key.StartsWith(TEXT("row/")) || Key.StartsWith(TEXT("inner/"))) && IsEntityKey(Key)))
			return TEXT("Present");

		return FNamer{Base, Ours, Theirs}.Value(Key, Value);
	}

	FConflictSummary SummarizeGroup(const FMergePlan& Plan, int32 Group, const FSnapshot& Base, const FSnapshot& Ours, const FSnapshot& Theirs)
	{
		const FNamer Names{Base, Ours, Theirs};
		FConflictSummary Summary;

		TArray<const FDecision*> Members;
		for (const FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Group == Group)
				Members.Add(&Decision);
		}

		const int32 HeadIndex = GroupHead(Plan, Group);
		if (HeadIndex == INDEX_NONE)
			return Summary;

		const FDecision& Head = Plan.Decisions[HeadIndex];

		auto JoinLimited = [](const TArray<FString>& Items)
		{
			constexpr int32 MaxItems = 3;
			TArray<FString> Shown(Items.GetData(), FMath::Min(Items.Num(), MaxItems));
			FString Text = FString::Join(Shown, TEXT(", "));
			if (Items.Num() > MaxItems)
				Text += FString::Printf(TEXT(" and %d more"), Items.Num() - MaxItems);
			return Text;
		};

		// Both sides added a variable with the same name.
		const FDecision* AddedByOurs = nullptr;
		const FDecision* AddedByTheirs = nullptr;
		for (const FDecision* Member : Members)
		{
			if (Member->Key.StartsWith(TEXT("var/")) && IsEntityKey(Member->Key) && !Member->Base.IsSet())
				(Member->Ours.IsSet() ? AddedByOurs : AddedByTheirs) = Member;
		}

		if (AddedByOurs && AddedByTheirs)
		{
			const FString Name = VariableField(AddedByOurs->Ours, 0);
			const FDecision* const* Default = Members.FindByPredicate([&Name](const FDecision* Member) { return Member->Key == TEXT("cdo/") + Name; });
			auto Side = [&](const FDecision* Variable, bool bOurs)
			{
				FString Text = TEXT("Added it as ") + VariableField(bOurs ? Variable->Ours : Variable->Theirs, 1);
				if (Default)
					Text += TEXT(", default ") + Names.Value((*Default)->Key, bOurs ? (*Default)->Ours : (*Default)->Theirs);
				return Text;
			};

			Summary.Title = FString::Printf(TEXT("Variable '%s' was added on both sides"), *Name);
			Summary.Ours = Side(AddedByOurs, true);
			Summary.Theirs = Side(AddedByTheirs, false);
			return Summary;
		}

		// One side removed something the other side changed (or, for a variable, still uses).
		if ((IsEntityKey(Head.Key) || Head.Key.StartsWith(TEXT("graph/"))) && Head.Base.IsSet() && Head.Ours.IsSet() != Head.Theirs.IsSet())
		{
			const bool bOursRemoved = !Head.Ours.IsSet();
			TArray<FString> Edits;
			for (const FDecision* Member : Members)
			{
				const TOptional<FString>& Value = bOursRemoved ? Member->Theirs : Member->Ours;
				FString Src, Dst;
				const bool bWhole = IsEntityKey(Member->Key) || SplitLink(Member->Key, Src, Dst);
				// Details of what the keeping side added are covered by "added ...".
				if (Member != &Head && Value != Member->Base && (bWhole || Member->Base.IsSet()))
					Edits.Add(Names.Edit(*Member, Value));
			}

			const TCHAR* Verb = Head.Key.StartsWith(TEXT("var/")) ? TEXT("Kept it and uses it: ") : TEXT("Kept it and changed it: ");
			const FString Changed = Edits.IsEmpty() ? FString(TEXT("Kept it")) : Verb + JoinLimited(Edits);
			Summary.Title = Names.Subject(Head.Key);
			Summary.Ours = bOursRemoved ? FString(TEXT("Removed it")) : Changed;
			Summary.Theirs = bOursRemoved ? Changed : FString(TEXT("Removed it"));
			return Summary;
		}

		// Links competing for a pin that takes one connection, with whatever nodes a side put in there.
		TMap<FString, TPair<bool, bool>> SidesAtPin;
		for (const FDecision* Member : Members)
		{
			FString Src, Dst;
			if (!SplitLink(Member->Key, Src, Dst))
				continue;
			for (const FString& End : {Src, Dst})
			{
				TPair<bool, bool>& Sides = SidesAtPin.FindOrAdd(End, TPair<bool, bool>(false, false));
				Sides.Key |= Member->Ours.IsSet();
				Sides.Value |= Member->Theirs.IsSet();
			}
		}

		FString Contested;
		for (const TPair<FString, TPair<bool, bool>>& Pin : SidesAtPin)
		{
			const bool bSingleLink = Base.SingleLinkPins.Contains(Pin.Key) || Ours.SingleLinkPins.Contains(Pin.Key) || Theirs.SingleLinkPins.Contains(Pin.Key);
			if (bSingleLink && Pin.Value.Key && Pin.Value.Value)
			{
				Contested = Pin.Key;
				break;
			}
		}

		if (!Contested.IsEmpty())
		{
			const bool bOutput = Contested.Contains(TEXT(":out:"));
			auto Side = [&](bool bOurs)
			{
				TArray<FString> Added, AddedIds;
				for (const FDecision* Member : Members)
				{
					const TOptional<FString>& Mine = bOurs ? Member->Ours : Member->Theirs;
					const TOptional<FString>& Other = bOurs ? Member->Theirs : Member->Ours;
					if (Member->Key.StartsWith(TEXT("node/")) && IsEntityKey(Member->Key) && !Member->Base.IsSet() && Mine.IsSet() && !Other.IsSet())
					{
						Added.Add(Names.Label(Member->Key.Mid(5)));
						AddedIds.Add(Member->Key.Mid(5));
					}
				}

				TArray<FString> Ends;
				bool bEndsAdded = true;
				for (const FDecision* Member : Members)
				{
					FString Src, Dst;
					if (!SplitLink(Member->Key, Src, Dst) || !(bOurs ? Member->Ours : Member->Theirs).IsSet() || (Src != Contested && Dst != Contested))
						continue;
					const FString End = Src == Contested ? Dst : Src;
					FString Node, Pin;
					SplitPinRef(End, Node, Pin);
					Ends.Add(Names.PinAt(End));
					bEndsAdded &= AddedIds.Contains(Node);
				}

				if (Ends.IsEmpty())
					return FString(TEXT("Not connected"));
				if (bEndsAdded && !Added.IsEmpty())
					return TEXT("Inserts ") + JoinLimited(Added);
				FString Text = (bOutput ? TEXT("Connects to ") : TEXT("Connected from ")) + JoinLimited(Ends);
				if (!Added.IsEmpty())
					Text += TEXT(", adding ") + JoinLimited(Added);
				return Text;
			};

			Summary.Title = Names.PinAt(Contested) + TEXT(" takes one connection");
			Summary.Ours = Side(true);
			Summary.Theirs = Side(false);
			return Summary;
		}

		// The same thing changed to different values.
		Summary.Title = Names.Subject(Head.Key);
		Summary.Ours = Names.Value(Head.Key, Head.Ours);
		Summary.Theirs = Names.Value(Head.Key, Head.Theirs);
		if (Members.Num() > 1)
			Summary.Title += FString::Printf(TEXT(" (+%d related)"), Members.Num() - 1);
		return Summary;
	}

	TArray<FString> NodeIdsOfKey(const FString& Key)
	{
		TArray<FString> Ids;
		FString Src, Dst, Node, Pin;
		if (SplitLink(Key, Src, Dst))
		{
			SplitPinRef(Src, Node, Pin);
			Ids.Add(Node);
			SplitPinRef(Dst, Node, Pin);
			Ids.AddUnique(Node);
		}
		else if (!NodeIdOfKey(Key).IsEmpty())
		{
			Ids.Add(NodeIdOfKey(Key));
		}
		return Ids;
	}

	const TCHAR* ToString(EResolution Resolution)
	{
		switch (Resolution)
		{
		case EResolution::Both: return TEXT("both");
		case EResolution::Ours: return TEXT("ours");
		case EResolution::Theirs: return TEXT("theirs");
		case EResolution::Conflict: return TEXT("conflict");
		case EResolution::BothTheirsFirst: return TEXT("both-theirs-first");
		default: return TEXT("unchanged");
		}
	}
}
