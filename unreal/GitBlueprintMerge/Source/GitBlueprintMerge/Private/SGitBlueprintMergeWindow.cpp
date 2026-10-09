// Copyright (c) MyGitGUI contributors. MIT License.

#include "SGitBlueprintMergeWindow.h"

#include "BlueprintMergeApply.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/Commands/UICommandList.h"
#include "GraphEditor.h"
#include "IDetailsView.h"
#include "IStructureDetailsView.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MergeSession.h"
#include "Misc/ITransaction.h"
#include "ScopedTransaction.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectHash.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyPath.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GitBlueprintMerge"

using namespace GitBlueprintMerge;

namespace
{
	// Same colors as MyGitGUI's conflict panel: ours blue, theirs orange.
	const FLinearColor OursColor(0.16f, 0.47f, 1.00f);
	const FLinearColor TheirsColor(0.96f, 0.58f, 0.13f);
	const FLinearColor BaseColor(0.45f, 0.45f, 0.45f);
	const FLinearColor BothColor(0.40f, 0.40f, 0.48f);
	const FLinearColor DecidedColor(0.22f, 0.72f, 0.33f);
	const FLinearColor OpenColor(0.96f, 0.42f, 0.20f);
	const FLinearColor ErrorColor(0.80f, 0.16f, 0.16f);

	// The result pane shows the merged result; it has no side of its own, so it borrows this value.
	constexpr EResolution ResultColumn = EResolution::Conflict;

	FLinearColor SideColor(EResolution Side)
	{
		switch (Side)
		{
		case EResolution::Ours: return OursColor;
		case EResolution::Theirs: return TheirsColor;
		case EResolution::Both: return BothColor;
		case ResultColumn: return DecidedColor;
		default: return BaseColor;
		}
	}

	const FSlateBrush* RoundedBrush()
	{
		static const FSlateRoundedBoxBrush Brush(FLinearColor::White, 4.0f);
		return &Brush;
	}

	TSharedRef<SWidget> MakeTag(const FText& Text, const FLinearColor& Color)
	{
		return SNew(SBorder)
			.BorderImage(RoundedBrush())
			.BorderBackgroundColor(Color)
			.Padding(FMargin(6.0f, 1.0f))
			.HAlign(HAlign_Center)
			[
				SNew(STextBlock)
				.Text(Text)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FLinearColor::White)
			];
	}

	UEdGraphNode* FindNodeById(const UBlueprint* Blueprint, const FString& Id)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid.ToString() == Id)
					return Node;
			}
		}
		return nullptr;
	}

	/**
	 * What to show where a version doesn't have something: "Removed in ours (was in Base)" tells a deliberate
	 * removal apart from something only the other side added.
	 */
	FText AbsentText(EResolution Side, bool bInBase)
	{
		if (Side == EResolution::Unchanged)
			return LOCTEXT("NotInBase", "Not in Base (added later)");
		if (!bInBase)
			return Side == ResultColumn ? LOCTEXT("NotInResult", "Not in the result") : LOCTEXT("NotHere", "Not in this version");
		if (Side == EResolution::Ours)
			return LOCTEXT("RemovedInOurs", "Removed in ours (was in Base)");
		if (Side == EResolution::Theirs)
			return LOCTEXT("RemovedInTheirs", "Removed in theirs (was in Base)");
		return LOCTEXT("RemovedInResult", "Removed (was in Base)");
	}

	UEdGraph* FindGraphById(const UBlueprint* Blueprint, const FString& Id)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && GraphId(Graph) == Id)
				return Graph;
		}
		return nullptr;
	}

	/** Property name of a key's property path ("A.B[1].C" -> "A"). */
	FName FirstProperty(FString Path)
	{
		int32 Cut = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), Cut))
			Path.LeftInline(Cut);
		if (Path.FindChar(TEXT('['), Cut))
			Path.LeftInline(Cut);
		return FName(*Path);
	}

	/** Something the Blueprint owns, by its path below the Blueprint ("inner/<path>" keys). */
	UObject* FindInner(UBlueprint* Blueprint, const FString& Path)
	{
		UObject* Found = nullptr;
		ForEachObjectWithOuter(Blueprint, [&Found, Blueprint, &Path](UObject* Object)
		{
			if (!Found && Object->GetPathName(Blueprint) == Path)
				Found = Object;
		});
		return Found;
	}
}

namespace
{
	/** Passes every mouse event on to the window (without using it up), for dragging nodes between graphs. */
	class FNodeDragProcessor : public IInputProcessor
	{
	public:
		explicit FNodeDragProcessor(const TWeakPtr<SGitBlueprintMergeWindow>& InWindow) : Window(InWindow) {}

		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

		virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
		{
			const TSharedPtr<SGitBlueprintMergeWindow> Pinned = Window.Pin();
			return Pinned.IsValid() && Pinned->OnGraphKeyDown(InKeyEvent);
		}

		virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
				if (const TSharedPtr<SGitBlueprintMergeWindow> Pinned = Window.Pin())
					Pinned->OnDragMouseDown(FVector2D(MouseEvent.GetScreenSpacePosition()));
			return false;
		}

		virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			if (const TSharedPtr<SGitBlueprintMergeWindow> Pinned = Window.Pin())
				Pinned->OnDragMouseMove(FVector2D(MouseEvent.GetScreenSpacePosition()));
			return false;
		}

		virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
				if (const TSharedPtr<SGitBlueprintMergeWindow> Pinned = Window.Pin())
					Pinned->OnDragMouseUp(FVector2D(MouseEvent.GetScreenSpacePosition()));
			return false;
		}

	private:
		TWeakPtr<SGitBlueprintMergeWindow> Window;
	};
}

SGitBlueprintMergeWindow::~SGitBlueprintMergeWindow()
{
	if (DragProcessor.IsValid() && FSlateApplication::IsInitialized())
		FSlateApplication::Get().UnregisterInputPreProcessor(DragProcessor);

	if (UTransBuffer* Buffer = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr)
		Buffer->OnTransactionStateChanged().RemoveAll(this);
}

void SGitBlueprintMergeWindow::Construct(const FArguments& InArgs)
{
	Session = InArgs._Session;
	OutputFile = InArgs._OutputFile;
	PackageName = InArgs._PackageName;
	OursLabel = InArgs._OursLabel;
	TheirsLabel = InArgs._TheirsLabel;
	OnDone = InArgs._OnDone;

	BuildRows();
	OnChoicesChanged();

	// Edits by hand in the result are transactions; any new one (or an undo) needs a new confirmation.
	if (UTransBuffer* Buffer = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr)
		Buffer->OnTransactionStateChanged().AddSP(this, &SGitBlueprintMergeWindow::OnTransactionStateChanged);

	// Nodes can be dragged from ours, base or theirs into the result.
	DragProcessor = MakeShared<FNodeDragProcessor>(StaticCastSharedRef<SGitBlueprintMergeWindow>(AsShared()));
	FSlateApplication::Get().RegisterInputPreProcessor(DragProcessor);

	const bool bHasConflicts = NumGroups() > 0;
	const FText AssetName = FText::FromString(Session->GetAssetName());

	TArray<FString> UnsupportedLines;
	for (const FString& Text : Session->GetUnsupported())
		UnsupportedLines.Add(TEXT("- ") + Text);

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(0.0f)
		[
			SNew(SVerticalBox)

			// Header: what this is, who is who, how far along we are.
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
				.Padding(FMargin(16.0f, 12.0f))
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::Format(LOCTEXT("Heading", "Merge {0}"), AssetName))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 15))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							MakeTag(LOCTEXT("OursTag", "OURS"), OursColor)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(6.0f, 0.0f, 18.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(SideName(EResolution::Ours))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							MakeTag(LOCTEXT("TheirsTag", "THEIRS"), TheirsColor)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(SideName(EResolution::Theirs))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(24.0f, 0.0f, 0.0f, 0.0f)
						[
							MakeLayoutSwitcher()
						]
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(bHasConflicts
							? LOCTEXT("SubtitleConflicts", "Both branches changed the same things. Pick a side for each conflict; everything else is merged for you.")
							: LOCTEXT("SubtitleClean", "No conflicts. Press Merge to combine both sides."))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 10.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						.Visibility(bHasConflicts ? EVisibility::Visible : EVisibility::Collapsed)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SBox)
							.WidthOverride(280.0f)
							.HeightOverride(8.0f)
							[
								SNew(SProgressBar)
								.Percent_Lambda([this]() { return TOptional<float>(NumGroups() > 0 ? float(Choices.Num()) / NumGroups() : 1.0f); })
								.FillColorAndOpacity_Lambda([this]() { return FSlateColor(AllDecided() ? DecidedColor : OpenColor); })
							]
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(10.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(this, &SGitBlueprintMergeWindow::GetProgressText)
							.ColorAndOpacity_Lambda([this]() { return CanMerge() ? FSlateColor(DecidedColor) : FSlateColor::UseForeground(); })
						]
					]
				]
			]

			// Merge failed: say why.
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(ErrorColor.CopyWithNewOpacity(0.35f))
				.Padding(FMargin(16.0f, 8.0f))
				.Visibility_Lambda([this]() { return Problems.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Top)
					.Padding(0.0f, 1.0f, 8.0f, 0.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.ErrorWithColor"))
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text_Lambda([this]() { return FText::Format(LOCTEXT("MergeFailed", "Could not merge: {0}"), FText::FromString(Problems)); })
					]
				]
			]

			// Changes the merge can't take yet: say so up front, and let the person leave them out.
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(OpenColor.CopyWithNewOpacity(0.18f))
				.Padding(FMargin(16.0f, 10.0f))
				.Visibility(UnsupportedLines.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Top)
					.Padding(0.0f, 1.0f, 8.0f, 0.0f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.WarningWithColor"))
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(SVerticalBox)

						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
							.Text(FText::Format(LOCTEXT("UnsupportedTitle", "{0} change(s) from {1} can't be merged automatically yet"),
								UnsupportedLines.Num(), SideName(EResolution::Theirs)))
						]

						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 4.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.Text(FText::FromString(FString::Join(UnsupportedLines, TEXT("\n"))))
						]

						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 6.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text(LOCTEXT("UnsupportedHelp", "Merge can leave them out: the result keeps ours there, and you redo them by hand in Unreal afterwards. "
								"Or cancel and keep one whole side in MyGitGUI (Take Ours / Take Theirs)."))
						]

						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 6.0f, 0.0f, 0.0f)
						[
							SNew(SCheckBox)
							.Visibility_Lambda([this]() { return bNeedsLeaveOut ? EVisibility::Visible : EVisibility::Collapsed; })
							.IsChecked_Lambda([this]() { return bLeaveOut ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
							{
								bLeaveOut = State == ECheckBoxState::Checked;
								Problems.Reset();
							})
							[
								SNew(STextBlock)
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
								.Text(LOCTEXT("LeaveOut", "Leave these out, I'll redo them by hand"))
							]
						]

						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 6.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Visibility_Lambda([this]() { return bNeedsLeaveOut ? EVisibility::Collapsed : EVisibility::Visible; })
							.ColorAndOpacity(DecidedColor)
							.Text(LOCTEXT("NothingLeftOut", "With your current choices none of these are taken, so nothing is left out."))
						]
					]
				]
			]

			// Closing with decisions made: confirm inline instead of a modal dialog.
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(OpenColor.CopyWithNewOpacity(0.30f))
				.Padding(FMargin(16.0f, 8.0f))
				.Visibility_Lambda([this]() { return bConfirmingClose ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text_Lambda([this]()
						{
							return FText::Format(LOCTEXT("ConfirmClose", "Close without merging? Your {0} decision(s) and {1} edit(s) by hand will be lost. Nothing in your project changes."), Choices.Num(), NumEdits());
						})
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("CloseAnyway", "Close without merging"))
						.OnClicked_Lambda([this]()
						{
							bFinished = true;
							OnDone.ExecuteIfBound();
							return FReply::Handled();
						})
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("KeepWorking", "Keep working"))
						.OnClicked_Lambda([this]()
						{
							bConfirmingClose = false;
							return FReply::Handled();
						})
					]
				]
			]

			// A new choice would build the result again over the edits by hand: ask first.
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				MakeDiscardBar()
			]

			// The result was edited by hand: confirm, undo or revert.
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				MakeEditsBar()
			]

			// The main area; its arrangement is the chosen layout (see MakeLayout).
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(8.0f)
			[
				SAssignNew(MainArea, SBox)
			]

			// Footer: navigation, bulk choices, merge.
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
				.Padding(FMargin(12.0f, 8.0f))
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.IsEnabled(bHasConflicts)
						.ToolTipText(LOCTEXT("PrevTip", "Previous conflict"))
						.OnClicked_Lambda([this]() { SelectConflict(-1, false); return FReply::Handled(); })
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SImage).Image(FAppStyle::GetBrush("Icons.ChevronLeft")).ColorAndOpacity(FSlateColor::UseForeground())
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(STextBlock).Text(LOCTEXT("Previous", "Previous"))
							]
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.IsEnabled(bHasConflicts)
						.ToolTipText(LOCTEXT("NextTip", "Next conflict"))
						.OnClicked_Lambda([this]() { SelectConflict(1, false); return FReply::Handled(); })
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(STextBlock).Text(LOCTEXT("Next", "Next"))
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(SImage).Image(FAppStyle::GetBrush("Icons.ChevronRight")).ColorAndOpacity(FSlateColor::UseForeground())
							]
						]
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNullWidget::NullWidget
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 16.0f, 0.0f)
					[
						SNew(SCheckBox)
						.ToolTipText(LOCTEXT("BaseTip", "Also show the version both branches started from"))
						.IsChecked_Lambda([this]() { return bShowBase ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bShowBase = State == ECheckBoxState::Checked; })
						[
							SNew(STextBlock).Text(LOCTEXT("ShowBase", "Show base"))
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("AllOurs", "Keep all ours"))
						.IsEnabled(bHasConflicts)
						.OnClicked_Lambda([this]() { ChooseAll(EResolution::Ours); return FReply::Handled(); })
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("AllTheirs", "Take all theirs"))
						.IsEnabled(bHasConflicts)
						.OnClicked_Lambda([this]() { ChooseAll(EResolution::Theirs); return FReply::Handled(); })
					]

					// Right next to Merge, so it's clear why Merge is off.
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(24.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SCheckBox)
						.Visibility_Lambda([this]() { return bNeedsLeaveOut ? EVisibility::Visible : EVisibility::Collapsed; })
						.ToolTipText(FText::FromString(FString::Join(UnsupportedLines, TEXT("\n"))))
						.IsChecked_Lambda([this]() { return bLeaveOut ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
						{
							bLeaveOut = State == ECheckBoxState::Checked;
							Problems.Reset();
						})
						[
							SNew(STextBlock)
							.Text(FText::Format(LOCTEXT("LeaveOutShort", "Leave out {0} unsupported change(s)"), UnsupportedLines.Num()))
							.ColorAndOpacity_Lambda([this]() { return bLeaveOut ? FSlateColor::UseForeground() : FSlateColor(OpenColor); })
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(16.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Cancel", "Cancel"))
						.ToolTipText(LOCTEXT("CancelTip", "Close without merging (Esc). Nothing in your project changes."))
						.OnClicked(this, &SGitBlueprintMergeWindow::OnCancel)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.IsEnabled(this, &SGitBlueprintMergeWindow::CanMerge)
						.ToolTipText_Lambda([this]()
						{
							if (!AllDecided())
								return FText::Format(LOCTEXT("MergeBlockedTip", "Decide every conflict first ({0} left)."), NumGroups() - Choices.Num());
							if (HasEdits() && !bEditsConfirmed)
								return LOCTEXT("MergeEditsTip", "Confirm your edits to the result first (bar at the top).");
							if (bConfirmingDiscard)
								return LOCTEXT("MergeDiscardTip", "Answer the question at the top first.");
							if (!CanMerge())
								return LOCTEXT("MergeLeaveOutTip", "Some changes can't be merged yet (listed at the top): tick \"Leave out\" first.");
							return LOCTEXT("MergeTip", "Merge, save and close");
						})
						.OnClicked(this, &SGitBlueprintMergeWindow::OnMerge)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("Merge", "Merge"))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
						]
					]
				]
			]
		]
	];

	// Lay out the main area as last time, then start on the first conflict (or the first change when there are none).
	LayoutIndex = FMath::Clamp(GConfig->GetIntOrDefault(TEXT("GitBlueprintMerge"), TEXT("Layout"), 0, GEditorPerProjectIni), 0, NumLayouts - 1);
	RebuildLayout();

	if (!SelectConflict(1, false))
	{
		if (ChangeRows.Num() > 0)
			ChangeList->SetSelection(ChangeRows[0]);
		else
			ShowRow(nullptr);
	}
}

void SGitBlueprintMergeWindow::BuildRows()
{
	const FMergePlan& Plan = Session->GetPlan();

	for (int32 Group = 0; Group < Plan.NumGroups; ++Group)
	{
		const FConflictSummary Summary = Session->Summarize(Group);
		TSharedPtr<FRow> Row = MakeShared<FRow>();
		Row->Kind = ERowKind::Conflict;
		Row->Group = Group;
		Row->Title = Summary.Title;
		Row->Ours = Summary.Ours;
		Row->Theirs = Summary.Theirs;
		Row->bCanTakeBoth = Session->CanTakeBoth(Group);

		TArray<FString> Details;
		for (const FDecision& Decision : Plan.Decisions)
		{
			if (Decision.Group != Group)
				continue;

			for (const FString& Id : NodeIdsOfKey(Decision.Key))
				Row->NodeIds.AddUnique(Id);
			if (!IsWorthShowing(Decision))
				continue;

			Details.AddUnique(TEXT("- ") + Session->Describe(Decision));

			// A variable's own default is usually empty; its class default says more.
			const bool bEmptyDefault = Decision.Key.EndsWith(TEXT("/default"))
				&& Decision.Base.Get(FString()).IsEmpty() && Decision.Ours.Get(FString()).IsEmpty() && Decision.Theirs.Get(FString()).IsEmpty();
			if (IsValueKey(Decision.Key) && !bEmptyDefault)
				Row->Values.Add(static_cast<int32>(&Decision - Plan.Decisions.GetData()));
		}

		// The summary already says it all for single-fact conflicts.
		if (Details.Num() > 1)
			Row->Details = FString::Join(Details, TEXT("\n"));

		ConflictRows.Add(Row);
	}

	for (const FDecision& Decision : Plan.Decisions)
	{
		if (Decision.Resolution == EResolution::Unchanged || Decision.Resolution == EResolution::Conflict || Decision.bPartOfWhole)
			continue;

		TSharedPtr<FRow> Row = MakeShared<FRow>();
		Row->Kind = ERowKind::Change;
		Row->Title = Session->Describe(Decision);
		Row->Resolution = Decision.Resolution;
		Row->NodeIds = NodeIdsOfKey(Decision.Key);
		if (IsValueKey(Decision.Key))
			Row->Values.Add(static_cast<int32>(&Decision - Plan.Decisions.GetData()));
		ChangeRows.Add(Row);
	}
}

void SGitBlueprintMergeWindow::SetLayout(int32 Index)
{
	if (Index == LayoutIndex || Index < 0 || Index >= NumLayouts)
		return;

	LayoutIndex = Index;
	GConfig->SetInt(TEXT("GitBlueprintMerge"), TEXT("Layout"), LayoutIndex, GEditorPerProjectIni);
	RebuildLayout();
}

void SGitBlueprintMergeWindow::RebuildLayout()
{
	// Cards sit side by side in the top strip, one under another in a side list.
	bCardsSideBySide = LayoutIndex == 0;
	MainArea->SetContent(MakeLayout(LayoutIndex));

	// The lists and views are new: put the selection back and fill the views.
	if (SelectedRow.IsValid())
	{
		const bool bConflict = SelectedRow->Kind == ERowKind::Conflict;
		const TSharedPtr<SListView<TSharedPtr<FRow>>>& Owner = bConflict ? List : ChangeList;
		Owner->SetSelection(SelectedRow);
		Owner->RequestScrollIntoView(SelectedRow);
	}
	ShowRow(SelectedRow.Get());
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeLayout(int32 Index)
{
	switch (Index)
	{
	case 1: // Side list: cards | ours | (base) | result | theirs
		return SNew(SSplitter).Orientation(Orient_Horizontal).PhysicalSplitterHandleSize(4.0f)
			+ SSplitter::Slot().Value(0.24f)[MakeChangesPanel()]
			+ SSplitter::Slot().Value(0.25f)[MakeGraphColumn(EResolution::Ours, OursBox)]
			+ SSplitter::Slot().Value(0.25f)[MakeBaseColumn()]
			+ SSplitter::Slot().Value(0.26f)[MakeGraphColumn(ResultColumn, ResultBox)]
			+ SSplitter::Slot().Value(0.25f)[MakeGraphColumn(EResolution::Theirs, TheirsBox)];

	case 2: // Classic three-way: cards | (ours | base | theirs over result)
		return SNew(SSplitter).Orientation(Orient_Horizontal).PhysicalSplitterHandleSize(4.0f)
			+ SSplitter::Slot().Value(0.26f)[MakeChangesPanel()]
			+ SSplitter::Slot().Value(0.74f)
			[
				SNew(SSplitter).Orientation(Orient_Vertical).PhysicalSplitterHandleSize(4.0f)
				+ SSplitter::Slot().Value(0.45f)
				[
					SNew(SSplitter).Orientation(Orient_Horizontal).PhysicalSplitterHandleSize(4.0f)
					+ SSplitter::Slot().Value(0.5f)[MakeGraphColumn(EResolution::Ours, OursBox)]
					+ SSplitter::Slot().Value(0.5f)[MakeBaseColumn()]
					+ SSplitter::Slot().Value(0.5f)[MakeGraphColumn(EResolution::Theirs, TheirsBox)]
				]
				+ SSplitter::Slot().Value(0.55f)[MakeGraphColumn(ResultColumn, ResultBox)]
			];

	case 3: // Result focus: cards | big result | ours, base, theirs stacked
		return SNew(SSplitter).Orientation(Orient_Horizontal).PhysicalSplitterHandleSize(4.0f)
			+ SSplitter::Slot().Value(0.24f)[MakeChangesPanel()]
			+ SSplitter::Slot().Value(0.50f)[MakeGraphColumn(ResultColumn, ResultBox)]
			+ SSplitter::Slot().Value(0.26f)
			[
				SNew(SSplitter).Orientation(Orient_Vertical).PhysicalSplitterHandleSize(4.0f)
				+ SSplitter::Slot().Value(0.5f)[MakeGraphColumn(EResolution::Ours, OursBox)]
				+ SSplitter::Slot().Value(0.5f)[MakeBaseColumn()]
				+ SSplitter::Slot().Value(0.5f)[MakeGraphColumn(EResolution::Theirs, TheirsBox)]
			];

	default: // Changes on top: cards strip, ours | (base) | theirs, result at the bottom
		return SNew(SSplitter).Orientation(Orient_Vertical).PhysicalSplitterHandleSize(4.0f)
			+ SSplitter::Slot().Value(0.30f)[MakeChangesPanel()]
			+ SSplitter::Slot().Value(0.35f)
			[
				SNew(SSplitter).Orientation(Orient_Horizontal).PhysicalSplitterHandleSize(4.0f)
				+ SSplitter::Slot().Value(0.5f)[MakeGraphColumn(EResolution::Ours, OursBox)]
				+ SSplitter::Slot().Value(0.5f)[MakeBaseColumn()]
				+ SSplitter::Slot().Value(0.5f)[MakeGraphColumn(EResolution::Theirs, TheirsBox)]
			]
			+ SSplitter::Slot().Value(0.35f)[MakeGraphColumn(ResultColumn, ResultBox)];
	}
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeBaseColumn()
{
	return SNew(SBox)
		.Visibility_Lambda([this]() { return bShowBase ? EVisibility::Visible : EVisibility::Collapsed; })
		[
			MakeGraphColumn(EResolution::Unchanged, BaseBox)
		];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeChangesPanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(6.0f)
		[
			MakeChangesSplitter()
		];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeChangesSplitter()
{
	// Conflicts and what merged on its own, both in view: stacked in a side list, next to each other in the top strip.
	const TSharedRef<SWidget> Conflicts = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeSectionHeader(FText::Format(LOCTEXT("ConflictsHeader", "Conflicts ({0})"), ConflictRows.Num()),
				LOCTEXT("ConflictsHeaderTip", "Both branches changed these; pick a side for each."))
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(List, SListView<TSharedPtr<FRow>>)
			.Orientation(bCardsSideBySide ? Orient_Horizontal : Orient_Vertical)
			.ListItemsSource(&ConflictRows)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SGitBlueprintMergeWindow::OnGenerateRow)
			.OnSelectionChanged(this, &SGitBlueprintMergeWindow::OnSelectionChanged)
		];

	const TSharedRef<SWidget> Changes = SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			MakeSectionHeader(FText::Format(LOCTEXT("ChangesHeader", "Merged automatically ({0})"), ChangeRows.Num()),
				LOCTEXT("ChangesHeaderTip", "Only one branch changed these (or both the same way); they are taken as is."))
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(ChangeList, SListView<TSharedPtr<FRow>>)
			.ListItemsSource(&ChangeRows)
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SGitBlueprintMergeWindow::OnGenerateRow)
			.OnSelectionChanged(this, &SGitBlueprintMergeWindow::OnSelectionChanged)
		];

	if (ConflictRows.Num() == 0)
		return Changes;
	if (ChangeRows.Num() == 0)
		return Conflicts;

	return SNew(SSplitter)
		.Orientation(bCardsSideBySide ? Orient_Horizontal : Orient_Vertical)
		.PhysicalSplitterHandleSize(4.0f)
		+ SSplitter::Slot().Value(bCardsSideBySide ? 0.65f : 0.5f)[Conflicts]
		+ SSplitter::Slot().Value(bCardsSideBySide ? 0.35f : 0.5f)[Changes];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeSectionHeader(const FText& Label, const FText& Tip) const
{
	return SNew(STextBlock)
		.Margin(FMargin(4.0f, 2.0f, 4.0f, 6.0f))
		.Text(Label)
		.ToolTipText(Tip)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10));
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeLayoutSwitcher()
{
	struct FLayoutInfo
	{
		FText Label;
		FText Tip;
	};
	const FLayoutInfo Layouts[NumLayouts] = {
		{LOCTEXT("LayoutTop", "Top"), LOCTEXT("LayoutTopTip", "Changes on top, ours and theirs side by side, result at the bottom")},
		{LOCTEXT("LayoutSide", "Side"), LOCTEXT("LayoutSideTip", "Changes on the left; ours, result and theirs in columns")},
		{LOCTEXT("LayoutClassic", "Classic"), LOCTEXT("LayoutClassicTip", "Changes on the left; ours and theirs above a wide result")},
		{LOCTEXT("LayoutFocus", "Result"), LOCTEXT("LayoutFocusTip", "Changes on the left, a big result, ours and theirs stacked on the right")},
	};

	TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Layout", "Layout"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];

	for (int32 Index = 0; Index < NumLayouts; ++Index)
	{
		Box->AddSlot()
			.AutoWidth()
			.Padding(2.0f, 0.0f)
			[
				SNew(SCheckBox)
				.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
				.Padding(FMargin(10.0f, 3.0f))
				.ToolTipText(Layouts[Index].Tip)
				.IsChecked_Lambda([this, Index]() { return LayoutIndex == Index ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState) { SetLayout(Index); })
				[
					SNew(STextBlock)
					.Text(Layouts[Index].Label)
				]
			];
	}

	return Box;
}

TSharedRef<ITableRow> SGitBlueprintMergeWindow::OnGenerateRow(TSharedPtr<FRow> Row, const TSharedRef<STableViewBase>& Owner)
{
	if (Row->Kind == ERowKind::Change)
	{
		return SNew(STableRow<TSharedPtr<FRow>>, Owner)
			.Padding(FMargin(4.0f, 3.0f))
			[
				MakeChangeRow(Row)
			];
	}

	// Side by side, cards have a fixed width and a long one scrolls inside; in a side list they fill its width.
	if (!bCardsSideBySide)
	{
		return SNew(STableRow<TSharedPtr<FRow>>, Owner)
			.Padding(FMargin(4.0f, 3.0f))
			[
				MakeConflictRow(Row)
			];
	}

	return SNew(STableRow<TSharedPtr<FRow>>, Owner)
		.Padding(FMargin(3.0f, 2.0f))
		[
			SNew(SBox)
			.WidthOverride(420.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					MakeConflictRow(Row)
				]
			]
		];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeConflictRow(const TSharedPtr<FRow>& Row)
{
	const int32 Group = Row->Group;

	return SNew(SBorder)
		.BorderImage(RoundedBrush())
		.BorderBackgroundColor_Lambda([this, Group]()
		{
			return FSlateColor((Choices.Contains(Group) ? DecidedColor : OpenColor).CopyWithNewOpacity(0.10f));
		})
		.Padding(FMargin(10.0f, 8.0f))
		[
			SNew(SHorizontalBox)

			// Decided (green check) or still open (warning).
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(0.0f, 1.0f, 8.0f, 0.0f)
			[
				SNew(SImage)
				.Image_Lambda([this, Group]()
				{
					return FAppStyle::GetBrush(Choices.Contains(Group) ? "Icons.SuccessWithColor" : "Icons.WarningWithColor");
				})
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Row->Title))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.AutoWrapText(true)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
						.Text_Lambda([this, Group]()
						{
							const EResolution* Choice = Choices.Find(Group);
							if (!Choice)
								return LOCTEXT("Open", "NOT DECIDED");
							if (IsBoth(Group))
								return LOCTEXT("TookBoth", "TOOK BOTH");
							return *Choice == EResolution::Ours ? LOCTEXT("KeptOurs", "KEPT OURS") : LOCTEXT("TookTheirs", "TOOK THEIRS");
						})
						.ColorAndOpacity_Lambda([this, Group]()
						{
							const EResolution* Choice = Choices.Find(Group);
							return FSlateColor(!Choice ? OpenColor : IsBoth(Group) ? DecidedColor : SideColor(*Choice));
						})
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					MakeSideLine(Group, EResolution::Ours, Row->Ours)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					MakeSideLine(Group, EResolution::Theirs, Row->Theirs)
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						MakeChoiceButton(Group, EResolution::Ours)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						MakeChoiceButton(Group, EResolution::Theirs)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						Row->bCanTakeBoth ? MakeChoiceButton(Group, EResolution::Both) : SNullWidget::NullWidget
					]
				]

				// With both taken: which side's nodes run first.
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					Row->bCanTakeBoth ? MakeOrderToggle(Group) : SNullWidget::NullWidget
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 6.0f, 0.0f, 0.0f)
				[
					SNew(SExpandableArea)
					.Visibility(Row->Details.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					.InitiallyCollapsed(true)
					.BorderImage(FStyleDefaults::GetNoBrush())
					.BodyBorderImage(FStyleDefaults::GetNoBrush())
					.AreaTitle(LOCTEXT("Details", "Details"))
					.AreaTitleFont(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.BodyContent()
					[
						SNew(STextBlock)
						.Text(FText::FromString(Row->Details))
						.AutoWrapText(true)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
			]
		];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeChangeRow(const TSharedPtr<FRow>& Row)
{
	const FText TagText = Row->Resolution == EResolution::Ours ? LOCTEXT("OursTag", "OURS")
		: Row->Resolution == EResolution::Theirs ? LOCTEXT("TheirsTag", "THEIRS")
		: LOCTEXT("BothTag", "BOTH");

	return SNew(SHorizontalBox)
		.ToolTipText(Row->Resolution == EResolution::Both
			? LOCTEXT("BothTip", "Both branches made this same change.")
			: FText::Format(LOCTEXT("SideTip", "Only {0} changed this; it is taken as is."), SideName(Row->Resolution)))

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Top)
		[
			SNew(SBox)
			.WidthOverride(56.0f)
			[
				MakeTag(TagText, SideColor(Row->Resolution))
			]
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Row->Title))
			.AutoWrapText(true)
		];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeSideLine(int32 Group, EResolution Side, const FString& Text)
{
	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Top)
		[
			SNew(SBox)
			.WidthOverride(56.0f)
			[
				MakeTag(Side == EResolution::Ours ? LOCTEXT("OursTag", "OURS") : LOCTEXT("TheirsTag", "THEIRS"), SideColor(Side))
			]
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(8.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Text))
			.AutoWrapText(true)
			.ColorAndOpacity_Lambda([this, Group, Side]()
			{
				// Fade the side that was not taken.
				return Choices.Contains(Group) && !Takes(Group, Side) ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground();
			})
		];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeChoiceButton(int32 Group, EResolution Side)
{
	const bool bBoth = Side == EResolution::Both;
	const FLinearColor Color = bBoth ? DecidedColor : SideColor(Side);
	const FText Label = Side == EResolution::Ours ? LOCTEXT("KeepOurs", "Keep ours")
		: Side == EResolution::Theirs ? LOCTEXT("TakeTheirs", "Take theirs")
		: LOCTEXT("TakeBoth", "Take both");
	const FText Tip = Side == EResolution::Ours ? LOCTEXT("KeepOursTip", "Keep our version")
		: Side == EResolution::Theirs ? LOCTEXT("TakeTheirsTip", "Take their version")
		: LOCTEXT("TakeBothTip", "Keep both sides' nodes, one after the other");

	// A real button: filled with the side's color once chosen, so the choice reads at a glance.
	auto IsSelected = [this, Group, Side, bBoth]() { return bBoth ? IsBoth(Group) : IsChosen(Group, Side); };

	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "Button")
		.ToolTipText(Tip)
		.ContentPadding(FMargin(14.0f, 5.0f))
		.ButtonColorAndOpacity_Lambda([IsSelected, Color]() { return FSlateColor(IsSelected() ? Color : FLinearColor(0.55f, 0.55f, 0.55f)); })
		.OnClicked_Lambda([this, Group, Side]()
		{
			Choose(Group, Side);
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)

			// Check when chosen, the side's color dot otherwise.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(12.0f)
				.HeightOverride(12.0f)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SWidgetSwitcher)
					.WidgetIndex_Lambda([IsSelected]() { return IsSelected() ? 1 : 0; })

					+ SWidgetSwitcher::Slot()
					[
						SNew(SBox)
						.WidthOverride(8.0f)
						.HeightOverride(8.0f)
						[
							SNew(SBorder)
							.BorderImage(RoundedBrush())
							.BorderBackgroundColor(Color)
						]
					]

					+ SWidgetSwitcher::Slot()
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Check"))
						.ColorAndOpacity(FLinearColor::White)
					]
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
				.ColorAndOpacity(FLinearColor::White)
			]
		];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeOrderToggle(int32 Group)
{
	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.Visibility_Lambda([this, Group]() { return IsBoth(Group) ? EVisibility::Visible : EVisibility::Collapsed; })
		.ToolTipText(LOCTEXT("SwapTip", "Swap which side's nodes run first"))
		.OnClicked_Lambda([this, Group]()
		{
			TMap<int32, EResolution> NewChoices = Choices;
			NewChoices.Add(Group, Choices.FindRef(Group) == EResolution::Both ? EResolution::BothTheirsFirst : EResolution::Both);
			SetChoices(NewChoices, false);
			return FReply::Handled();
		})
		[
			SNew(STextBlock)
			.ColorAndOpacity(DecidedColor)
			.Text_Lambda([this, Group]()
			{
				return Choices.FindRef(Group) == EResolution::Both
					? LOCTEXT("OursFirst", "Order: ours first, then theirs  (click to swap)")
					: LOCTEXT("TheirsFirst", "Order: theirs first, then ours  (click to swap)");
			})
		];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeGraphColumn(EResolution Side, TSharedPtr<SBox>& OutBox)
{
	const bool bBase = Side == EResolution::Unchanged;
	const bool bResult = Side == ResultColumn;
	const bool bChoosable = !bBase && !bResult;
	const FLinearColor Color = SideColor(Side);
	const FText Title = bBase ? LOCTEXT("BaseColumn", "BASE  (before both branches)")
		: bResult ? LOCTEXT("ResultColumn", "RESULT  (what Merge will save)")
		: FText::Format(LOCTEXT("SideColumn", "{0}  {1}"), Side == EResolution::Ours ? LOCTEXT("OursTag", "OURS") : LOCTEXT("TheirsTag", "THEIRS"), SideName(Side));

	return SNew(SBorder)
		.BorderImage(RoundedBrush())
		.Padding(2.0f)
		.BorderBackgroundColor_Lambda([this, Side, Color, bChoosable, bResult]()
		{
			// While nodes are dragged in, frame the result (brighter when they'd land in it).
			if (bResult && bDraggingNodes)
				return FSlateColor(DecidedColor.CopyWithNewOpacity(bDropOverResult ? 1.0f : 0.5f));
			// Frame the side(s) chosen for the selected conflict.
			return FSlateColor(bChoosable && Takes(GetSelectedGroup(), Side) ? Color : FLinearColor::Transparent);
		})
		[
			// Opaque inside, so the frame color only shows as a frame and the header stays readable.
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(0.0f)
			[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(Color.CopyWithNewOpacity(0.30f))
				.Padding(FMargin(10.0f, 6.0f))
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(Title)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]

					// Result: how to edit it by hand (and, while dragging, where to let go).
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Visibility(bResult ? EVisibility::Visible : EVisibility::Collapsed)
						.ToolTipText(LOCTEXT("EditableTip", "You can edit the result by hand: drag nodes in from ours or theirs, move nodes, drag wires between pins, "
							"change pin values and the details below the graph. Every edit can be undone; confirm your edits before you merge."))
						.ColorAndOpacity_Lambda([this]() { return bDraggingNodes ? FSlateColor(DecidedColor) : FSlateColor::UseSubduedForeground(); })
						.Font_Lambda([this]() { return FCoreStyle::GetDefaultFontStyle(bDraggingNodes ? "Bold" : "Italic", 9); })
						.Text_Lambda([this]()
						{
							if (bDraggingNodes)
								return bDropOverResult ? LOCTEXT("DropHere", "Let go to copy the node(s) here") : LOCTEXT("DragHere", "Drag the node(s) into this graph");
							return Session->GetPreview() ? LOCTEXT("EditableHintBP", "Editable: drag nodes here from ours / theirs") : LOCTEXT("EditableHint", "Editable: change values in the details below");
						})
					]

					// Blueprints: copy nodes selected in a version into the result, or delete nodes selected in the result.
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Visibility_Lambda([this]() { return Session->GetPreview() ? EVisibility::Visible : EVisibility::Collapsed; })
						.ToolTipText(bResult ? LOCTEXT("DeleteSelectedTip", "Delete the nodes selected in the result graph (\"Undo last edit\" brings them back)")
							: LOCTEXT("CopySelectedTip", "Copy the nodes selected in this graph into the result (same as dragging them down). Wires between the copied nodes are kept; connect the rest by hand."))
						.Text(bResult ? LOCTEXT("DeleteSelected", "Delete selected nodes") : LOCTEXT("CopyToResult", "Copy selected nodes to result"))
						.OnClicked_Lambda([this, Side, bResult]()
						{
							if (bResult)
								DeleteSelectedInResult();
							else
								CopySelectedToResult(Side);
							return FReply::Handled();
						})
					]

					// Result: keep both sides' nodes, for conflicts where that works.
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Visibility_Lambda([this, bResult]()
						{
							const FRow* Row = GetSelectedRow();
							return bResult && Row && Row->bCanTakeBoth ? EVisibility::Visible : EVisibility::Collapsed;
						})
						.ToolTipText(LOCTEXT("TakeBothTip", "Keep both sides' nodes, one after the other"))
						.ButtonColorAndOpacity_Lambda([this]() { return FSlateColor(IsBoth(GetSelectedGroup()) ? DecidedColor : FLinearColor(0.55f, 0.55f, 0.55f)); })
						.OnClicked_Lambda([this]()
						{
							Choose(GetSelectedGroup(), EResolution::Both);
							return FReply::Handled();
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("TakeBoth", "Take both"))
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
							.ColorAndOpacity(FLinearColor::White)
						]
					]

					// Result: whether it compiles, and what it still assumes.
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						.Visibility(bResult ? EVisibility::Visible : EVisibility::Collapsed)

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SImage)
							.Image_Lambda([this]()
							{
								if (HasEdits() && !bEditsConfirmed)
									return FAppStyle::GetBrush("Icons.WarningWithColor");
								return FAppStyle::GetBrush(Session->GetPreviewProblems().IsEmpty() ? "Icons.SuccessWithColor" : "Icons.ErrorWithColor");
							})
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(4.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(this, &SGitBlueprintMergeWindow::GetPreviewStatus)
							.ToolTipText_Lambda([this]() { return FText::FromString(FString::Join(Session->GetPreviewProblems(), TEXT("\n"))); })
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						.Visibility_Lambda([this, Side, bChoosable]() { return bChoosable && Takes(GetSelectedGroup(), Side) ? EVisibility::Visible : EVisibility::Collapsed; })

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.SuccessWithColor"))
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(4.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("Chosen", "Chosen"))
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.Visibility(bChoosable ? EVisibility::Visible : EVisibility::Collapsed)
						.IsEnabled_Lambda([this]() { return GetSelectedGroup() != INDEX_NONE; })
						.ToolTipText(Side == EResolution::Ours ? LOCTEXT("KeepOursTip", "Keep our version") : LOCTEXT("TakeTheirsTip", "Take their version"))
						.Text(Side == EResolution::Ours ? LOCTEXT("UseOurs", "Keep ours") : LOCTEXT("UseTheirs", "Take theirs"))
						.OnClicked_Lambda([this, Side]()
						{
							Choose(GetSelectedGroup(), Side);
							return FReply::Handled();
						})
					]
				]
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				// Dim the side that was not taken for the selected conflict.
				SNew(SBorder)
				.BorderImage(FStyleDefaults::GetNoBrush())
				.Padding(0.0f)
				.ColorAndOpacity_Lambda([this, Side, bChoosable]()
				{
					const int32 Group = GetSelectedGroup();
					const bool bOtherChosen = bChoosable && Choices.Contains(Group) && !Takes(Group, Side);
					return bOtherChosen ? FLinearColor(1.0f, 1.0f, 1.0f, 0.45f) : FLinearColor::White;
				})
				[
					SAssignNew(OutBox, SBox)
				]
			]
			]
		];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeGraphView(EResolution Side, UBlueprint* Blueprint, const TArray<FString>& NodeIds)
{
	TArray<UEdGraphNode*> Nodes;
	for (const FString& Id : NodeIds)
	{
		if (UEdGraphNode* Node = FindNodeById(Blueprint, Id))
			Nodes.Add(Node);
	}

	UEdGraph* Graph = Nodes.Num() > 0 ? Nodes[0]->GetGraph() : nullptr;

	// The nodes may exist only in other versions; show the same graph here.
	if (!Graph)
	{
		for (const UBlueprint* Other : {Session->GetOurs(), Session->GetTheirs(), Session->GetBase()})
		{
			for (const FString& Id : NodeIds)
			{
				if (const UEdGraphNode* OtherNode = FindNodeById(Other, Id))
				{
					Graph = FindGraphById(Blueprint, GraphId(OtherNode->GetGraph()));
					break;
				}
			}
			if (Graph)
				break;
		}
	}

	if (!Graph && Blueprint->UbergraphPages.Num() > 0)
		Graph = Blueprint->UbergraphPages[0];

	if (!Graph)
	{
		return SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("NoGraph", "No graph to show."))
			];
	}

	FGraphAppearanceInfo Appearance;
	const bool bInBase = Session->GetBase() && NodeIds.ContainsByPredicate([this](const FString& Id) { return FindNodeById(Session->GetBase(), Id) != nullptr; });
	Appearance.CornerText = NodeIds.Num() > 0 && Nodes.Num() == 0 ? AbsentText(Side, bInBase) : FText::GetEmpty();

	// The result can be edited by hand; the details of the nodes selected in it show (and edit) below the graph.
	const bool bResult = Side == ResultColumn;
	SGraphEditor::FGraphEditorEvents Events;
	if (bResult)
	{
		ResultNodeDetails = MakeObjectDetails(true);
		Events.OnSelectionChanged = SGraphEditor::FOnSelectionChanged::CreateLambda([this](const FGraphPanelSelectionSet& Selection)
		{
			if (ResultNodeDetails.IsValid())
				ResultNodeDetails->SetObjects(Selection.Array());
		});
	}

	TSharedRef<SGraphEditor> Editor = SNew(SGraphEditor)
		.GraphToEdit(Graph)
		.IsEditable(bResult)
		.DisplayAsReadOnly(!bResult)
		.ShowGraphStateOverlay(false)
		.Appearance(Appearance)
		.GraphEvents(Events)
		.AdditionalCommands(Session->GetPreview() ? MakeGraphCommands(Side) : TSharedPtr<FUICommandList>());
	GraphEditors.Add(Side, Editor);

	// Select everything the change touches and bring it into view.
	Nodes.RemoveAll([Graph](const UEdGraphNode* Node) { return Node->GetGraph() != Graph; });
	for (UEdGraphNode* Node : Nodes)
		Editor->SetNodeSelection(Node, true);

	if (Nodes.Num() == 1)
		Editor->JumpToNode(Nodes[0], false, false);
	else if (Nodes.Num() > 1)
		Editor->ZoomToFit(true);

	if (!bResult)
		return Editor;

	TArray<UObject*> Selected;
	for (UEdGraphNode* Node : Nodes)
		Selected.Add(Node);
	ResultNodeDetails->SetObjects(Selected);

	return SNew(SSplitter)
		.Orientation(Orient_Vertical)
		.PhysicalSplitterHandleSize(4.0f)
		+ SSplitter::Slot().Value(0.72f)[Editor]
		+ SSplitter::Slot().Value(0.28f)[ResultNodeDetails.ToSharedRef()];
}

void SGitBlueprintMergeWindow::OnSelectionChanged(TSharedPtr<FRow> Row, ESelectInfo::Type SelectInfo)
{
	if (!Row.IsValid() || Row == SelectedRow)
		return;

	// One selection across both lists.
	SelectedRow = Row;
	const TSharedPtr<SListView<TSharedPtr<FRow>>>& Other = Row->Kind == ERowKind::Conflict ? ChangeList : List;
	if (Other.IsValid())
		Other->ClearSelection();

	ShowRow(Row.Get());
}

void SGitBlueprintMergeWindow::ShowRow(const FRow* Row)
{
	OursBox->SetContent(MakeSideView(EResolution::Ours, Row));
	TheirsBox->SetContent(MakeSideView(EResolution::Theirs, Row));
	BaseBox->SetContent(MakeSideView(EResolution::Unchanged, Row));
	ResultBox->SetContent(MakeSideView(ResultColumn, Row));
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeSideView(EResolution Side, const FRow* Row)
{
	GraphEditors.Remove(Side);
	GraphCommands.Remove(Side);
	RowViews.Remove(Side);
	if (Side == ResultColumn)
	{
		ResultNodeDetails.Reset();
		ResultTable.Reset();
	}

	UObject* Asset = Side == EResolution::Ours ? Session->GetOursAsset()
		: Side == EResolution::Theirs ? Session->GetTheirsAsset()
		: Side == ResultColumn ? Session->GetPreviewAsset()
		: Session->GetBaseAsset();
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);

	if (!Asset)
	{
		return SNew(SBox)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(16.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(FText::Format(LOCTEXT("NoPreview", "No preview: {0}"), FText::FromString(FString::Join(Session->GetPreviewProblems(), TEXT(" ")))))
			];
	}

	const bool bHasValues = Row && Row->Values.Num() > 0;
	const bool bHasNodes = Row && Row->NodeIds.Num() > 0;

	TSharedRef<SVerticalBox> View = SNew(SVerticalBox);
	if (bHasValues)
	{
		View->AddSlot()
			.AutoHeight()
			[
				MakeValuePanel(Side, *Row)
			];
	}

	if (Blueprint && (bHasNodes || !bHasValues))
	{
		View->AddSlot()
			.FillHeight(1.0f)
			[
				MakeGraphView(Side, Blueprint, bHasNodes ? Row->NodeIds : TArray<FString>())
			];
	}
	else if (const TSharedPtr<SWidget> Details = Row ? MakeDetailsView(Side, Asset, *Row) : nullptr)
	{
		View->AddSlot()
			.FillHeight(1.0f)
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				Details.ToSharedRef()
			];
	}
	else
	{
		View->AddSlot()
			.FillHeight(1.0f)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.Padding(16.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(!bHasValues ? LOCTEXT("PickChange", "Pick a change in the list to compare it.")
					: Blueprint ? LOCTEXT("NoGraphChange", "Not a graph change: the values above are all there is to compare.")
					: LOCTEXT("NoDetails", "The values above are all there is to compare."))
				.AutoWrapText(true)
			];
	}

	return View;
}

TSharedPtr<SWidget> SGitBlueprintMergeWindow::MakeDetailsView(EResolution Side, UObject* Asset, const FRow& Row)
{
	// Class defaults for variables and class default properties, the component template for components, the
	// Blueprint for Class Settings, the widget (or its slot) for widgets; the asset itself for properties and
	// parameters of data assets and material instances; the row for data tables.
	const bool bEditable = Side == ResultColumn;
	UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	UDataTable* Table = Cast<UDataTable>(Asset);
	UObject* Object = nullptr;
	FName PropertyName;
	FName RowName;
	for (int32 Index : Row.Values)
	{
		const FString& Key = Session->GetPlan().Decisions[Index].Key;
		UObject* Found = nullptr;
		FName FoundName;

		if (Table)
		{
			if (Key.StartsWith(TEXT("row/")))
			{
				FString Id = Key.Mid(4), Field;
				Key.Mid(4).Split(TEXT("/"), &Id, &Field);
				RowName = FName(*Id.Replace(TEXT("%2F"), TEXT("/")).Replace(TEXT("%25"), TEXT("%")));
				break;
			}
			continue;
		}

		if (!Blueprint)
		{
			FString Name;
			if (Key.StartsWith(TEXT("prop/")))
				Name = Key.Mid(5);
			else if (Key.StartsWith(TEXT("param/")))
				Key.Mid(6).Split(TEXT("/"), &Name, nullptr);
			if (!Name.IsEmpty())
			{
				Found = Asset;
				FoundName = FirstProperty(Name);
			}
		}
		else if (Key.StartsWith(TEXT("cdo/")))
		{
			Found = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
			FoundName = FName(*Key.Mid(4));
		}
		else if (Key.StartsWith(TEXT("bp/")))
		{
			Found = Blueprint;
			FoundName = FirstProperty(Key.Mid(3));
		}
		else if (Key.StartsWith(TEXT("inner/")))
		{
			FString Path = Key.Mid(6), Property;
			Key.Mid(6).Split(TEXT("/"), &Path, &Property);
			Found = FindInner(Blueprint, Path);
			if (Property.StartsWith(TEXT("Slot.")))
			{
				const UWidget* Widget = Cast<UWidget>(Found);
				Found = Widget ? Widget->Slot : nullptr;
				Property = Property.Mid(5);
			}
			if (!Property.IsEmpty())
				FoundName = FirstProperty(Property);
		}
		else if (Key.StartsWith(TEXT("var/")))
		{
			FString Id = Key.Mid(4), Detail;
			Key.Mid(4).Split(TEXT("/"), &Id, &Detail);
			Found = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				if (Variable.VarGuid.ToString() == Id)
					FoundName = Variable.VarName;
			}
		}
		else if (Key.StartsWith(TEXT("component/")))
		{
			FString Id = Key.Mid(10), Detail, Property;
			Key.Mid(10).Split(TEXT("/"), &Id, &Detail);
			Detail.Split(TEXT("prop/"), nullptr, &Property);
			if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
			{
				for (USCS_Node* Node : SCS->GetAllNodes())
				{
					if (Node && Node->VariableGuid.ToString() == Id)
						Found = Node->ComponentTemplate;
				}
			}
			if (!Property.IsEmpty())
				FoundName = FName(*Property);
		}

		if (!Object)
			Object = Found;
		if (Found == Object && PropertyName.IsNone())
			PropertyName = FoundName;
		if (Object && !PropertyName.IsNone())
			break;
	}

	// A data table row: its struct, edited in place (NotifyPreChange records the table for undo).
	if (Table)
	{
		uint8* RowData = RowName.IsNone() ? nullptr : Table->FindRowUnchecked(RowName);
		if (!RowData || !Table->GetRowStruct())
			return nullptr;

		FDetailsViewArgs Args;
		Args.bAllowSearch = true;
		Args.bHideSelectionTip = true;
		Args.bShowOptions = false;
		Args.bAllowFavoriteSystem = false;
		Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		Args.NotifyHook = bEditable ? this : nullptr;

		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		const TSharedRef<IStructureDetailsView> View = PropertyEditor.CreateStructureDetailView(Args, FStructureDetailsViewArgs(),
			MakeShared<FStructOnScope>(Table->GetRowStruct(), RowData), FText::FromName(RowName));
		View->GetDetailsView()->SetIsPropertyEditingEnabledDelegate(FIsPropertyEditingEnabled::CreateLambda([bEditable]() { return bEditable; }));
		RowViews.Add(Side, View);
		if (bEditable)
		{
			ResultTable = Table;
			ResultRowName = RowName;
		}
		return View->GetWidget();
	}

	if (!Object)
		return nullptr;

	const TSharedRef<IDetailsView> Details = MakeObjectDetails(bEditable);
	Details->SetObject(Object);

	// Point at the property in question; the rows exist after the first tick, so scroll then.
	if (const FProperty* Property = PropertyName.IsNone() ? nullptr : FindFProperty<FProperty>(Object->GetClass(), PropertyName))
	{
		const TSharedRef<FPropertyPath> Path = FPropertyPath::Create(const_cast<FProperty*>(Property));
		Details->HighlightProperty(*Path);

		const TWeakPtr<IDetailsView> WeakDetails = Details;
		Details->RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda([WeakDetails, Path](double, float)
		{
			if (const TSharedPtr<IDetailsView> View = WeakDetails.Pin())
				View->ScrollPropertyIntoView(*Path, true);
			return EActiveTimerReturnType::Stop;
		}));
	}

	return Details;
}

TSharedRef<IDetailsView> SGitBlueprintMergeWindow::MakeObjectDetails(bool bEditable) const
{
	FDetailsViewArgs Args;
	Args.bAllowSearch = true;
	Args.bHideSelectionTip = true;
	Args.bShowPropertyMatrixButton = false;
	Args.bShowOptions = false;
	Args.bAllowFavoriteSystem = false;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;

	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	const TSharedRef<IDetailsView> Details = PropertyEditor.CreateDetailView(Args);
	Details->SetIsPropertyEditingEnabledDelegate(FIsPropertyEditingEnabled::CreateLambda([bEditable]() { return bEditable; }));
	return Details;
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeValuePanel(EResolution Side, const FRow& Row) const
{
	const FMergePlan& Plan = Session->GetPlan();

	// One line per thing; facts that read the same (e.g. the two 'Debug' variables) share a line.
	struct FLine
	{
		FString Subject;
		TOptional<FString> Value;
		FString Key;
		TArray<FString> Keys;
		bool bChanged = false;
		bool bInBase = false;
	};
	TArray<FLine> Lines;
	for (int32 Index : Row.Values)
	{
		const FDecision& Decision = Plan.Decisions[Index];
		const FString* InResult = Side == ResultColumn ? Session->GetPreviewSnapshot().Facts.Find(Decision.Key) : nullptr;
		const TOptional<FString> Value = Side == EResolution::Ours ? Decision.Ours
			: Side == EResolution::Theirs ? Decision.Theirs
			: Side == ResultColumn ? (InResult ? TOptional<FString>(*InResult) : TOptional<FString>())
			: Decision.Base;
		const FString Subject = Session->Subject(Decision.Key);

		FLine* Line = Lines.FindByPredicate([&Subject](const FLine& Existing) { return Existing.Subject == Subject; });
		if (!Line)
		{
			Line = &Lines.AddDefaulted_GetRef();
			Line->Subject = Subject;
		}
		Line->Keys.Add(Decision.Key);
		if (!Line->Value.IsSet() && Value.IsSet())
		{
			Line->Value = Value;
			Line->Key = Decision.Key;
		}
		Line->bChanged |= Side != EResolution::Unchanged && Value != Decision.Base;
		Line->bInBase |= Decision.Base.IsSet();
	}

	const FLinearColor Color = SideColor(Side);
	TSharedRef<SVerticalBox> Box = SNew(SVerticalBox);
	for (const FLine& Line : Lines)
	{
		Box->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 3.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Line.Subject))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text_Lambda([this, Side, Line]()
					{
						// The result follows edits by hand, so read it from the preview each time.
						if (Side == ResultColumn)
						{
							for (const FString& Key : Line.Keys)
							{
								if (const FString* Value = Session->GetPreviewSnapshot().Facts.Find(Key))
									return FText::FromString(Session->ValueText(Key, TOptional<FString>(*Value)));
							}
							return AbsentText(Side, Line.bInBase);
						}
						return Line.Value.IsSet() ? FText::FromString(Session->ValueText(Line.Key, Line.Value)) : AbsentText(Side, Line.bInBase);
					})
					.Font(FCoreStyle::GetDefaultFontStyle(Line.Value.IsSet() ? "Bold" : "Italic", 13))
					.ColorAndOpacity(Line.bChanged ? FSlateColor(FMath::Lerp(Color, FLinearColor::White, 0.35f)) : FSlateColor::UseForeground())
					.AutoWrapText(true)
				]
			];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(FMargin(14.0f, 10.0f))
		[
			Box
		];
}

void SGitBlueprintMergeWindow::Choose(int32 Group, EResolution Side)
{
	if (Group == INDEX_NONE)
		return;

	TMap<int32, EResolution> NewChoices = Choices;
	NewChoices.Add(Group, Side);

	// Go on to the next open conflict, so the conflicts can be worked through in one pass.
	SetChoices(NewChoices, true);
}

void SGitBlueprintMergeWindow::ChooseAll(EResolution Side)
{
	TMap<int32, EResolution> NewChoices = Choices;
	for (int32 Group = 0; Group < NumGroups(); ++Group)
		NewChoices.Add(Group, Side);

	SetChoices(NewChoices, false);
}

void SGitBlueprintMergeWindow::SetChoices(const TMap<int32, EResolution>& NewChoices, bool bAdvance)
{
	// The result is built again for new choices, which would drop the edits by hand: ask first.
	if (HasEdits())
	{
		PendingChoices = NewChoices;
		bPendingAdvance = bAdvance;
		bConfirmingDiscard = true;
		return;
	}

	Choices = NewChoices;
	OnChoicesChanged();
	if (bAdvance)
		SelectConflict(1, true);
}

void SGitBlueprintMergeWindow::OnChoicesChanged()
{
	Problems.Reset();
	EditsMessage.Reset();
	bConfirmingClose = false;
	bConfirmingDiscard = false;
	bConfirmingRevert = false;
	bEditsConfirmed = false;
	bNeedsLeaveOut = Session->NeedsLeaveOut(Choices);

	// Rebuild the result with these choices and show it. It starts without edits by hand, so nothing to undo.
	bInternalChange = true;
	Session->UpdatePreview(Choices);
	if (GEditor)
		GEditor->ResetTransaction(LOCTEXT("ResultRebuilt", "Merge result built again"));
	bInternalChange = false;

	RefreshResult();
}

void SGitBlueprintMergeWindow::RefreshResult()
{
	if (ResultBox.IsValid())
		ResultBox->SetContent(MakeSideView(ResultColumn, GetSelectedRow()));
}

int32 SGitBlueprintMergeWindow::NumEdits() const
{
	const UTransactor* Transactor = GEditor ? GEditor->Trans.Get() : nullptr;
	return Transactor ? FMath::Max(0, Transactor->GetQueueLength() - Transactor->GetUndoCount()) : 0;
}

void SGitBlueprintMergeWindow::OnTransactionStateChanged(const FTransactionContext& Context, ETransactionStateEventType State)
{
	if (bInternalChange || (State != ETransactionStateEventType::TransactionFinalized && State != ETransactionStateEventType::UndoRedoFinalized))
		return;

	// Something changed in the result: what was confirmed no longer is.
	bEditsConfirmed = false;
	bConfirmingClose = false;
	bConfirmingRevert = false;
	EditsMessage.Reset();
	Problems.Reset();
	Session->RefreshPreviewSnapshot();
}

void SGitBlueprintMergeWindow::UndoEdit()
{
	if (!HasEdits() || !GEditor)
		return;

	GEditor->UndoTransaction();
	AfterUndoRedo();
}

void SGitBlueprintMergeWindow::RedoEdit()
{
	if (!GEditor || !GEditor->Trans || !GEditor->Trans->CanRedo())
		return;

	GEditor->RedoTransaction();
	AfterUndoRedo();
}

void SGitBlueprintMergeWindow::AfterUndoRedo()
{
	// The views are built again; keep the keyboard on the result graph when it was there (Ctrl+Z pressed twice).
	const TSharedPtr<SGraphEditor> Before = GraphEditors.FindRef(ResultColumn).Pin();
	const bool bHadFocus = Before.IsValid() && Before->HasFocusedDescendants();

	Session->RefreshPreviewSnapshot();
	RefreshResult();

	if (const TSharedPtr<SGraphEditor> After = GraphEditors.FindRef(ResultColumn).Pin(); bHadFocus && After.IsValid())
		FSlateApplication::Get().SetKeyboardFocus(After, EFocusCause::SetDirectly);
}

void SGitBlueprintMergeWindow::CopyToClipboard(EResolution Side)
{
	const TSharedPtr<SGraphEditor> Editor = GraphEditors.FindRef(Side).Pin();
	if (!Editor.IsValid())
		return;

	ClipboardNodes.Reset();
	for (UObject* Object : Editor->GetSelectedNodes())
	{
		if (UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
			ClipboardNodes.Add(Node);
	}
	ClipboardSource = Side == ResultColumn ? Session->GetPreview()
		: Side == EResolution::Ours ? Session->GetOurs()
		: Side == EResolution::Theirs ? Session->GetTheirs()
		: Session->GetBase();
}

void SGitBlueprintMergeWindow::PasteFromClipboard()
{
	TArray<UEdGraphNode*> Nodes;
	for (const TWeakObjectPtr<UEdGraphNode>& Node : ClipboardNodes)
	{
		if (Node.IsValid())
			Nodes.Add(Node.Get());
	}

	const TSharedPtr<SGraphEditor> Result = GraphEditors.FindRef(ResultColumn).Pin();
	if (Nodes.IsEmpty() || !ClipboardSource.IsValid() || !Result.IsValid())
	{
		EditsMessage = TEXT("Nothing to paste: select nodes in a graph and press Ctrl+C first.");
		return;
	}

	// Where the mouse last was in the result graph.
	const FVector2D At = FVector2D(Result->GetPasteLocation2f());
	CopyNodesToResult(ClipboardSource.Get(), Nodes, &At);
}

bool SGitBlueprintMergeWindow::OnGraphKeyDown(const FKeyEvent& InKeyEvent)
{
	// Typing in a pin value or a details field keeps its own Ctrl+C / Delete / Ctrl+Z.
	if (const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetKeyboardFocusedWidget())
	{
		const FString Type = Focused->GetTypeAsString();
		if (Type.Contains(TEXT("EditableText")) || Type.Contains(TEXT("TextBox")))
			return false;
	}

	// The graphs don't take keyboard focus on a click, so the shortcuts go to the graph under the mouse.
	const FVector2D Cursor = FSlateApplication::Get().GetCursorPos();
	for (const EResolution Side : {ResultColumn, EResolution::Ours, EResolution::Theirs, EResolution::Unchanged})
	{
		if (Side == EResolution::Unchanged && !bShowBase)
			continue;

		const TSharedPtr<SGraphEditor> Editor = GraphEditors.FindRef(Side).Pin();
		const TSharedPtr<FUICommandList> Commands = GraphCommands.FindRef(Side);
		if (Editor.IsValid() && Commands.IsValid() && Editor->GetTickSpaceGeometry().IsUnderLocation(Cursor))
			return Commands->ProcessCommandBindings(InKeyEvent);
	}
	return false;
}

TSharedRef<FUICommandList> SGitBlueprintMergeWindow::MakeGraphCommands(EResolution Side)
{
	// Graph editing only: copy anywhere; paste, delete, undo and redo in the result. Everything else stays on buttons.
	const TSharedRef<FUICommandList> Commands = MakeShared<FUICommandList>();
	GraphCommands.Add(Side, Commands);
	Commands->MapAction(FGenericCommands::Get().Copy, FExecuteAction::CreateLambda([this, Side]() { CopyToClipboard(Side); }));
	if (Side != ResultColumn)
		return Commands;

	Commands->MapAction(FGenericCommands::Get().Paste, FExecuteAction::CreateLambda([this]() { PasteFromClipboard(); }));
	Commands->MapAction(FGenericCommands::Get().Delete, FExecuteAction::CreateLambda([this]() { DeleteSelectedInResult(); }));
	Commands->MapAction(FGenericCommands::Get().Undo, FExecuteAction::CreateLambda([this]() { UndoEdit(); }));
	Commands->MapAction(FGenericCommands::Get().Redo, FExecuteAction::CreateLambda([this]() { RedoEdit(); }));
	return Commands;
}

void SGitBlueprintMergeWindow::RevertEdits()
{
	// Back to the result as the choices make it.
	OnChoicesChanged();
}

void SGitBlueprintMergeWindow::ConfirmEdits()
{
	bInternalChange = true;
	const bool bCompiles = Session->CheckEditedPreview();
	bInternalChange = false;

	bEditsConfirmed = bCompiles;
	EditsMessage = bCompiles ? FString() : TEXT("Not confirmed, the result doesn't compile: ") + FString::Join(Session->GetPreviewProblems(), TEXT(" "));
	RefreshResult();
}

void SGitBlueprintMergeWindow::CopySelectedToResult(EResolution Side, const FVector2D* DropAt)
{
	UBlueprint* Source = Side == EResolution::Ours ? Session->GetOurs()
		: Side == EResolution::Theirs ? Session->GetTheirs()
		: Session->GetBase();
	UBlueprint* Target = Session->GetPreview();
	const TSharedPtr<SGraphEditor> Editor = GraphEditors.FindRef(Side).Pin();
	if (!Source || !Target || !Editor.IsValid())
		return;

	TArray<UEdGraphNode*> Nodes;
	for (UObject* Object : Editor->GetSelectedNodes())
	{
		if (UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
			Nodes.Add(Node);
	}
	if (Nodes.IsEmpty())
	{
		EditsMessage = TEXT("Select the nodes to copy in that graph first.");
		return;
	}

	CopyNodesToResult(Source, Nodes, DropAt);
}

void SGitBlueprintMergeWindow::CopyNodesToResult(UBlueprint* Source, const TArray<UEdGraphNode*>& Nodes, const FVector2D* DropAt)
{
	UBlueprint* Target = Session->GetPreview();
	if (!Source || !Target || Nodes.IsEmpty())
		return;

	// From the button: put the copies in the middle of what the result graph shows, not on top of existing nodes.
	const TSharedPtr<SGraphEditor> Result = GraphEditors.FindRef(ResultColumn).Pin();
	FVector2D Center;
	if (!DropAt && Result.IsValid())
	{
		float Zoom = 1.0f;
		Result->GetViewLocation(Center, Zoom);
		Center += FVector2D(Result->GetTickSpaceGeometry().GetLocalSize()) * 0.5 / FMath::Max(Zoom, 0.01f);
		DropAt = &Center;
	}

	FString Problem;
	TArray<UEdGraphNode*> Copies;
	{
		const FScopedTransaction Transaction(LOCTEXT("CopyToResultTransaction", "Copy nodes into the merge result"));
		Copies = GitBlueprintMerge::CopyNodes(Nodes, Source, Target, Problem, DropAt);
	}
	if (!Problem.IsEmpty())
		EditsMessage = Problem;
	else if (Copies.IsEmpty())
		EditsMessage = TEXT("Nothing was copied.");

	// Redraw the result graph with the copies in it, selected.
	if (Result.IsValid())
	{
		Result->NotifyGraphChanged();
		Result->ClearSelectionSet();
		for (UEdGraphNode* Copy : Copies)
		{
			if (Copy->GetGraph() == Result->GetCurrentGraph())
				Result->SetNodeSelection(Copy, true);
		}
	}
}

void SGitBlueprintMergeWindow::OnDragMouseDown(const FVector2D& ScreenPos)
{
	DragSide.Reset();
	bDraggingNodes = false;
	if (!Session->GetPreview())
		return;

	// Only a press on a node (or its pins) in a version's graph starts a drag; elsewhere it's a pan or a box select.
	const FWidgetPath Path = FSlateApplication::Get().LocateWindowUnderMouse(ScreenPos, FSlateApplication::Get().GetInteractiveTopLevelWindows());
	bool bOnNode = false;
	for (int32 Index = 0; Index < Path.Widgets.Num() && !bOnNode; ++Index)
		bOnNode = Path.Widgets[Index].Widget->GetTypeAsString().StartsWith(TEXT("SGraphNode"));
	if (!bOnNode)
		return;

	for (const EResolution Side : {EResolution::Ours, EResolution::Theirs, EResolution::Unchanged})
	{
		const TSharedPtr<SGraphEditor> Editor = GraphEditors.FindRef(Side).Pin();
		if (Editor.IsValid() && Path.ContainsWidget(Editor.Get()))
		{
			DragSide = Side;
			DragStart = ScreenPos;
			return;
		}
	}
}

void SGitBlueprintMergeWindow::OnDragMouseMove(const FVector2D& ScreenPos)
{
	if (DragSide.IsSet() && !bDraggingNodes && FVector2D::Distance(ScreenPos, DragStart) > FSlateApplication::Get().GetDragTriggerDistance())
		bDraggingNodes = true;
	if (bDraggingNodes)
		bDropOverResult = IsOverResultGraph(ScreenPos);
}

void SGitBlueprintMergeWindow::OnDragMouseUp(const FVector2D& ScreenPos)
{
	const bool bDrop = bDraggingNodes && DragSide.IsSet() && IsOverResultGraph(ScreenPos);
	const EResolution Side = DragSide.Get(EResolution::Unchanged);
	DragSide.Reset();
	bDraggingNodes = false;
	bDropOverResult = false;
	if (!bDrop)
		return;

	// Where it was dropped, in result graph coordinates.
	const TSharedPtr<SGraphEditor> Result = GraphEditors.FindRef(ResultColumn).Pin();
	FVector2D View;
	float Zoom = 1.0f;
	Result->GetViewLocation(View, Zoom);
	const FVector2D Local = FVector2D(Result->GetTickSpaceGeometry().AbsoluteToLocal(ScreenPos));
	const FVector2D DropAt = View + Local / FMath::Max(Zoom, 0.01f);

	// After this event the source graph handles the release; copy on the next tick, when its selection is final.
	const TWeakPtr<SGitBlueprintMergeWindow> Weak = StaticCastSharedRef<SGitBlueprintMergeWindow>(AsShared());
	RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateLambda([Weak, Side, DropAt](double, float)
	{
		if (const TSharedPtr<SGitBlueprintMergeWindow> Window = Weak.Pin())
			Window->CopySelectedToResult(Side, &DropAt);
		return EActiveTimerReturnType::Stop;
	}));
}

bool SGitBlueprintMergeWindow::IsOverResultGraph(const FVector2D& ScreenPos) const
{
	const TSharedPtr<SGraphEditor> Result = GraphEditors.FindRef(ResultColumn).Pin();
	return Result.IsValid() && Result->GetTickSpaceGeometry().IsUnderLocation(ScreenPos);
}

void SGitBlueprintMergeWindow::DeleteSelectedInResult()
{
	UBlueprint* Blueprint = Session->GetPreview();
	const TSharedPtr<SGraphEditor> Editor = GraphEditors.FindRef(ResultColumn).Pin();
	if (!Blueprint || !Editor.IsValid())
		return;

	TArray<UEdGraphNode*> Nodes;
	for (UObject* Object : Editor->GetSelectedNodes())
	{
		UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
		if (Node && Node->CanUserDeleteNode())
			Nodes.Add(Node);
	}
	if (Nodes.IsEmpty())
	{
		EditsMessage = TEXT("Select nodes in the result graph to delete first (some nodes, like function entries, can't be deleted).");
		return;
	}

	{
		const FScopedTransaction Transaction(LOCTEXT("DeleteInResultTransaction", "Delete nodes from the merge result"));
		for (UEdGraphNode* Node : Nodes)
		{
			Node->GetGraph()->Modify();
			Node->Modify();
			FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
		}
	}

	Editor->NotifyGraphChanged();
	Editor->ClearSelectionSet();
	if (ResultNodeDetails.IsValid())
		ResultNodeDetails->SetObjects(TArray<UObject*>());
}

void SGitBlueprintMergeWindow::NotifyPreChange(FProperty* PropertyAboutToChange)
{
	// Data table rows are plain memory to the details view; record the table so the edit can be undone.
	UDataTable* Table = ResultTable.Get();
	if (!Table || bInRowEdit || !GEditor)
		return;

	bInRowEdit = true;
	GEditor->BeginTransaction(LOCTEXT("EditRowTransaction", "Edit a row of the merge result"));
	Table->Modify();
}

void SGitBlueprintMergeWindow::NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged)
{
	if (!bInRowEdit)
		return;

	bInRowEdit = false;
	if (UDataTable* Table = ResultTable.Get())
		Table->HandleDataTableChanged(ResultRowName);
	if (GEditor)
		GEditor->EndTransaction();
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeDiscardBar()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(OpenColor.CopyWithNewOpacity(0.30f))
		.Padding(FMargin(16.0f, 8.0f))
		.Visibility_Lambda([this]() { return bConfirmingDiscard ? EVisibility::Visible : EVisibility::Collapsed; })
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this]()
				{
					return FText::Format(LOCTEXT("ConfirmDiscard", "A new choice builds the result again from ours and theirs: your {0} edit(s) by hand will be lost."), NumEdits());
				})
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("DropEdits", "Drop my edits and apply the choice"))
				.OnClicked_Lambda([this]()
				{
					Choices = PendingChoices;
					OnChoicesChanged();
					if (bPendingAdvance)
						SelectConflict(1, true);
					return FReply::Handled();
				})
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("KeepEdits", "Keep my edits"))
				.OnClicked_Lambda([this]()
				{
					bConfirmingDiscard = false;
					return FReply::Handled();
				})
			]
		];
}

TSharedRef<SWidget> SGitBlueprintMergeWindow::MakeEditsBar()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([this]() { return FSlateColor((bEditsConfirmed ? DecidedColor : OpenColor).CopyWithNewOpacity(0.22f)); })
		.Padding(FMargin(16.0f, 8.0f))
		.Visibility_Lambda([this]() { return HasEdits() || !EditsMessage.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(0.0f, 1.0f, 8.0f, 0.0f)
			[
				SNew(SImage)
				.Image_Lambda([this]() { return FAppStyle::GetBrush(bEditsConfirmed ? "Icons.SuccessWithColor" : "Icons.WarningWithColor"); })
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility_Lambda([this]() { return HasEdits() ? EVisibility::Visible : EVisibility::Collapsed; })
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.Text_Lambda([this]()
					{
						if (bConfirmingRevert)
							return FText::Format(LOCTEXT("ConfirmRevert", "Drop all {0} edit(s) by hand and go back to the result your choices make? This can't be undone."), NumEdits());
						return bEditsConfirmed
							? FText::Format(LOCTEXT("EditsConfirmed", "Your {0} edit(s) by hand are confirmed. Merge saves the result as you left it."), NumEdits())
							: FText::Format(LOCTEXT("EditsOpen", "You changed the result by hand ({0} change(s)). When it looks right, press \"Confirm my edits\"; Merge stays off until then. "
								"\"Undo last edit\" takes back your last change, \"Revert all edits\" throws all of them away."), NumEdits());
					})
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility_Lambda([this]() { return EditsMessage.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.ColorAndOpacity(FMath::Lerp(ErrorColor, FLinearColor::White, 0.35f))
					.Text_Lambda([this]() { return FText::FromString(EditsMessage); })
				]
			]

			// Normal state: confirm, undo one, revert all.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this]() { return HasEdits() && !bConfirmingRevert ? EVisibility::Visible : EVisibility::Collapsed; })

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Visibility_Lambda([this]() { return bEditsConfirmed ? EVisibility::Collapsed : EVisibility::Visible; })
					.ToolTipText(LOCTEXT("ConfirmEditsTip", "Check the edited result (Blueprints are compiled) and accept it for Merge"))
					.OnClicked_Lambda([this]() { ConfirmEdits(); return FReply::Handled(); })
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ConfirmEdits", "Confirm my edits"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("UndoEdit", "Undo last edit"))
					.ToolTipText(LOCTEXT("UndoEditTip", "Take back the last edit by hand in the result"))
					.OnClicked_Lambda([this]() { UndoEdit(); return FReply::Handled(); })
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("RevertEdits", "Revert all edits"))
					.ToolTipText(LOCTEXT("RevertEditsTip", "Drop every edit by hand: the result goes back to what your choices make"))
					.OnClicked_Lambda([this]() { bConfirmingRevert = true; return FReply::Handled(); })
				]
			]

			// Reverting can't be undone: ask once more.
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([this]() { return HasEdits() && bConfirmingRevert ? EVisibility::Visible : EVisibility::Collapsed; })

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("RevertYes", "Revert all edits"))
					.OnClicked_Lambda([this]() { RevertEdits(); return FReply::Handled(); })
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("RevertNo", "Keep my edits"))
					.OnClicked_Lambda([this]() { bConfirmingRevert = false; return FReply::Handled(); })
				]
			]
		];
}

const SGitBlueprintMergeWindow::FRow* SGitBlueprintMergeWindow::GetSelectedRow() const
{
	return SelectedRow.Get();
}

bool SGitBlueprintMergeWindow::IsBoth(int32 Group) const
{
	const EResolution* Choice = Choices.Find(Group);
	return Choice && (*Choice == EResolution::Both || *Choice == EResolution::BothTheirsFirst);
}

bool SGitBlueprintMergeWindow::Takes(int32 Group, EResolution Side) const
{
	return IsChosen(Group, Side) || IsBoth(Group);
}

FText SGitBlueprintMergeWindow::GetPreviewStatus() const
{
	if (HasEdits() && !bEditsConfirmed)
		return LOCTEXT("PreviewEdited", "Edited by hand: confirm your edits to check it");
	if (!Session->GetPreviewProblems().IsEmpty())
		return LOCTEXT("PreviewBroken", "Won't merge as is (hover for why)");

	const int32 Open = NumGroups() - Choices.Num();
	return Open > 0
		? FText::Format(LOCTEXT("PreviewOpen", "Compiles. {0} undecided shown as ours"), Open)
		: LOCTEXT("PreviewDone", "Compiles");
}

bool SGitBlueprintMergeWindow::SelectConflict(int32 Direction, bool bOnlyOpen)
{
	const int32 Count = ConflictRows.Num();
	if (Count == 0)
		return false;

	const int32 Current = GetSelectedGroup();
	const int32 Start = Current != INDEX_NONE ? Current : (Direction > 0 ? Count - 1 : 0);
	for (int32 Step = 1; Step <= Count; ++Step)
	{
		const int32 Candidate = ((Start + Direction * Step) % Count + Count) % Count;
		if (bOnlyOpen && Choices.Contains(Candidate))
			continue;

		const TSharedPtr<FRow>& Row = ConflictRows[Candidate];
		List->SetSelection(Row);
		List->RequestScrollIntoView(Row);
		return true;
	}

	return false;
}

int32 SGitBlueprintMergeWindow::GetSelectedGroup() const
{
	return SelectedRow.IsValid() && SelectedRow->Kind == ERowKind::Conflict ? SelectedRow->Group : INDEX_NONE;
}

bool SGitBlueprintMergeWindow::IsChosen(int32 Group, EResolution Side) const
{
	const EResolution* Choice = Choices.Find(Group);
	return Choice && *Choice == Side;
}

FText SGitBlueprintMergeWindow::SideName(EResolution Side) const
{
	const FString& Label = Side == EResolution::Ours ? OursLabel : TheirsLabel;
	if (!Label.IsEmpty())
		return FText::FromString(Label);
	return Side == EResolution::Ours ? LOCTEXT("YourBranch", "your branch") : LOCTEXT("Incoming", "incoming");
}

int32 SGitBlueprintMergeWindow::NumGroups() const
{
	return ConflictRows.Num();
}

FText SGitBlueprintMergeWindow::GetProgressText() const
{
	if (CanMerge())
		return FText::Format(LOCTEXT("AllDecided", "All {0} conflict(s) decided. Press Merge."), NumGroups());
	if (AllDecided() && HasEdits() && !bEditsConfirmed)
		return FText::Format(LOCTEXT("AllDecidedEdits", "All {0} conflict(s) decided. Confirm your edits to the result, then press Merge."), NumGroups());
	if (AllDecided())
		return FText::Format(LOCTEXT("AllDecidedLeaveOut", "All {0} conflict(s) decided. To merge, tick \"Leave out\" next to the Merge button."), NumGroups());
	return FText::Format(LOCTEXT("Decided", "{0} of {1} conflict(s) decided"), Choices.Num(), NumGroups());
}

bool SGitBlueprintMergeWindow::AllDecided() const
{
	return Choices.Num() >= NumGroups();
}

bool SGitBlueprintMergeWindow::CanMerge() const
{
	return AllDecided() && (!bNeedsLeaveOut || bLeaveOut) && (!HasEdits() || bEditsConfirmed) && !bConfirmingDiscard;
}

FReply SGitBlueprintMergeWindow::OnMerge()
{
	if (!CanMerge())
		return FReply::Handled();

	bConfirmingClose = false;
	bInternalChange = true;
	const bool bMerged = HasEdits()
		? Session->MergeEdited(Choices, OutputFile, PackageName, bNeedsLeaveOut && bLeaveOut)
		: Session->Merge(Choices, OutputFile, PackageName, bNeedsLeaveOut && bLeaveOut);
	bInternalChange = false;
	if (bMerged)
	{
		bFinished = true;
		OnDone.ExecuteIfBound();
	}
	else
	{
		Problems = FString::Join(Session->GetProblems(), TEXT(" "));
	}

	return FReply::Handled();
}

FReply SGitBlueprintMergeWindow::OnCancel()
{
	if (ConfirmClose())
	{
		bFinished = true;
		OnDone.ExecuteIfBound();
	}
	return FReply::Handled();
}

bool SGitBlueprintMergeWindow::ConfirmClose()
{
	if (bFinished || (Choices.Num() == 0 && !HasEdits()))
		return true;

	bConfirmingClose = true;
	return false;
}

#undef LOCTEXT_NAMESPACE
