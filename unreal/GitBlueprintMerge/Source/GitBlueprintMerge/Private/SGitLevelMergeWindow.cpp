// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#include "SGitLevelMergeWindow.h"

#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GitLevelMerge"

using namespace GitBlueprintMerge;

namespace
{
	const FSlateColor Dim(FLinearColor(0.55f, 0.55f, 0.6f));
	const FSlateColor OursColor(FLinearColor::FromSRGBColor(FColor(0x4C, 0x9A, 0xFF)));
	const FSlateColor TheirsColor(FLinearColor::FromSRGBColor(FColor(0xF0, 0xA3, 0x3A)));
	const FSlateColor BothColor(FLinearColor::FromSRGBColor(FColor(0xE5, 0x53, 0x4B)));
	const FSlateColor WarnColor(FLinearColor::FromSRGBColor(FColor(0xFF, 0xB0, 0x20)));
	const FSlateColor DoneColor(FLinearColor::FromSRGBColor(FColor(0x3C, 0xB3, 0x71)));

	/** Property | Change | Base | Ours | Theirs | In level, then the fixed Use column. */
	const float ColumnFill[] = {0.2f, 0.12f, 0.15f, 0.15f, 0.15f, 0.23f};
	const float UseWidth = 250.0f;

	FString Shorten(const FString& Text)
	{
		return Text.Len() > 60 ? Text.Left(57) + TEXT("...") : Text;
	}

	/** One value cell: shortened text, full value in the tooltip. */
	TSharedRef<SWidget> ValueCell(const FString& Value, bool bPresent, const FSlateColor& Color)
	{
		const FString Shown = !bPresent ? TEXT("-") : (Value.IsEmpty() ? TEXT("(empty)") : Value);
		return SNew(STextBlock)
			.Text(FText::FromString(Shorten(Shown)))
			.ToolTipText(FText::FromString(Shown))
			.ColorAndOpacity(Color);
	}

	TSharedRef<SWidget> Columns(const TArray<TSharedRef<SWidget>>& Cells, const TSharedRef<SWidget>& Use)
	{
		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		for (int32 Index = 0; Index < Cells.Num(); ++Index)
			Row->AddSlot().FillWidth(ColumnFill[Index]).VAlign(VAlign_Center).Padding(0, 0, 8, 0)[Cells[Index]];
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(SBox).WidthOverride(UseWidth)[Use]];
		return Row;
	}

	TSharedRef<SWidget> Warning(const TAttribute<FText>& Text)
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(SImage).Image(FAppStyle::GetBrush("Icons.Warning")).ColorAndOpacity(WarnColor)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Text).ColorAndOpacity(WarnColor).AutoWrapText(true)
			];
	}

	void KindText(ELevelChangeKind Kind, FText& OutText, FText& OutTip, FSlateColor& OutColor)
	{
		switch (Kind)
		{
		case ELevelChangeKind::Both:
			OutText = LOCTEXT("KindBoth", "Both changed");
			OutTip = LOCTEXT("KindBothTip", "Both sides changed this, to different values");
			OutColor = BothColor;
			break;
		case ELevelChangeKind::Same:
			OutText = LOCTEXT("KindSame", "Same on both");
			OutTip = LOCTEXT("KindSameTip", "Both sides made the same change");
			OutColor = Dim;
			break;
		case ELevelChangeKind::OursOnly:
			OutText = LOCTEXT("KindOurs", "Ours changed");
			OutTip = LOCTEXT("KindOursTip", "Only Ours changed this");
			OutColor = OursColor;
			break;
		default:
			OutText = LOCTEXT("KindTheirs", "Theirs changed");
			OutTip = LOCTEXT("KindTheirsTip", "Only Theirs changed this");
			OutColor = TheirsColor;
			break;
		}
	}
}

void SGitLevelMergeWindow::Construct(const FArguments& InArgs)
{
	Session = InArgs._Session;
	OnDone = InArgs._OnDone;

	// Actors changed on both sides first, then the ones one side deleted.
	const TArray<FLevelActor>& Actors = Session->GetActors();
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bExistence = Pass == 1;
		bool bHeader = false;
		for (int32 Index = 0; Index < Actors.Num(); ++Index)
		{
			if (Actors[Index].bExistenceConflict != bExistence)
				continue;
			if (!bHeader)
			{
				TSharedPtr<FActorItem> Header = MakeShared<FActorItem>();
				Header->Header = bExistence ? LOCTEXT("SectionDeleted", "DELETED ON ONE SIDE") : LOCTEXT("SectionChanged", "CHANGED ON BOTH SIDES");
				ActorItems.Add(Header);
				bHeader = true;
			}
			TSharedPtr<FActorItem> Item = MakeShared<FActorItem>();
			Item->ActorIndex = Index;
			ActorItems.Add(Item);
		}
	}

	TSharedRef<SVerticalBox> ProblemsBox = SNew(SVerticalBox);
	for (const FString& Problem : Session->GetProblems())
		ProblemsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)[Warning(FText::FromString(Problem))];

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SVerticalBox)

			// Header
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
					.Text(FText::Format(LOCTEXT("Header", "Merge level {0}"), FText::FromString(FPackageName::GetShortName(Session->GetLevel()))))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16, 0, 0, 0)
				[
					SNew(STextBlock).ColorAndOpacity(OursColor)
					.Text(FText::Format(LOCTEXT("OursLabel", "Ours: {0}"), FText::FromString(Session->GetOursLabel())))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 0, 0)
				[
					SNew(STextBlock).ColorAndOpacity(TheirsColor)
					.Text(FText::Format(LOCTEXT("TheirsLabel", "Theirs: {0}"), FText::FromString(Session->GetTheirsLabel())))
				]
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock).ColorAndOpacity(Dim).AutoWrapText(true)
				.Text(LOCTEXT("Hint", "Pick a side for every change; nothing is chosen for you. Neither keeps the level's current value, which you can edit in Details."))
			]

			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				ProblemsBox
			]

			// Whole level
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock).Text(LOCTEXT("WholeLevel", "Whole level:"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakeBulkChoices(INDEX_NONE, FString(), true)
				]
			]

			// Actors | changes
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SSplitter)
				+ SSplitter::Slot().Value(0.25f)
				[
					SAssignNew(ActorList, SListView<TSharedPtr<FActorItem>>)
					.ListItemsSource(&ActorItems)
					.SelectionMode(ESelectionMode::Single)
					.OnGenerateRow(this, &SGitLevelMergeWindow::MakeActorRow)
					.OnSelectionChanged(this, &SGitLevelMergeWindow::OnActorSelected)
					.OnIsSelectableOrNavigable_Lambda([](TSharedPtr<FActorItem> Item) { return Item.IsValid() && Item->ActorIndex != INDEX_NONE; })
				]
				+ SSplitter::Slot().Value(0.75f)
				[
					SAssignNew(Details, SBox).Padding(FMargin(12, 0, 0, 0))
				]
			]

			// Footer
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(this, &SGitLevelMergeWindow::GetProgressText)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(16, 0, 0, 0)
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return GetWarningText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						Warning(TAttribute<FText>(this, &SGitLevelMergeWindow::GetWarningText))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Cancel", "Cancel"))
					.ToolTipText(LOCTEXT("CancelTip", "Close without merging; the actors stay conflicted"))
					.OnClicked(this, &SGitLevelMergeWindow::OnCancel)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.Text(LOCTEXT("Merge", "Merge"))
					.ToolTipText_Lambda([this]()
					{
						return Session->IsAllDecided() ? LOCTEXT("MergeTip", "Save the actors as decided; GitO stages them")
							: LOCTEXT("MergeOffTip", "Decide every change first");
					})
					.IsEnabled_Lambda([this]() { return Session->IsAllDecided(); })
					.OnClicked(this, &SGitLevelMergeWindow::OnMerge)
				]
			]
		]
	];

	for (const TSharedPtr<FActorItem>& Item : ActorItems)
	{
		if (Item->ActorIndex != INDEX_NONE)
		{
			ActorList->SetSelection(Item);
			break;
		}
	}
}

bool SGitLevelMergeWindow::ConfirmClose() const
{
	if (Session->WasWritten() || Session->NumDecided() == 0)
		return true;

	return FMessageDialog::Open(EAppMsgType::YesNo,
		LOCTEXT("ConfirmClose", "Close without merging?\n\nYour choices are dropped and the actors stay conflicted."),
		LOCTEXT("ConfirmCloseTitle", "Merge level")) == EAppReturnType::Yes;
}

TSharedRef<ITableRow> SGitLevelMergeWindow::MakeActorRow(TSharedPtr<FActorItem> Item, const TSharedRef<STableViewBase>& Owner)
{
	if (Item->ActorIndex == INDEX_NONE)
	{
		return SNew(STableRow<TSharedPtr<FActorItem>>, Owner)
			.ShowSelection(false)
			.Padding(FMargin(6, 10, 6, 2))
			[
				SNew(STextBlock).Text(Item->Header).ColorAndOpacity(Dim).Font(FAppStyle::GetFontStyle("SmallFontBold"))
			];
	}

	const int32 ActorIndex = Item->ActorIndex;
	const FLevelActor& Actor = Session->GetActors()[ActorIndex];

	FText Kind;
	if (!Actor.bExistenceConflict)
		Kind = FText::Format(LOCTEXT("Changes", "{0} change(s)"), Actor.Changes.Num());
	else if (!Actor.bInTheirs)
		Kind = LOCTEXT("DeletedTheirs", "Deleted in Theirs");
	else
		Kind = LOCTEXT("DeletedOurs", "Deleted in Ours");

	const int32 NumBoth = Actor.NumBothChanged();

	return SNew(STableRow<TSharedPtr<FActorItem>>, Owner)
		.Padding(FMargin(6, 4))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(FText::FromString(Actor.Label)).Font(FAppStyle::GetFontStyle("BoldFont"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(STextBlock).ColorAndOpacity(Dim)
						.Text(FText::Format(LOCTEXT("ActorKind", "{0} · {1}"), FText::FromString(Actor.ClassName), Kind))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(STextBlock).ColorAndOpacity(BothColor)
						.Visibility(NumBoth > 0 ? EVisibility::Visible : EVisibility::Collapsed)
						.Text(FText::Format(LOCTEXT("ActorBoth", " · {0} on both sides"), NumBoth))
					]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this, ActorIndex]()
				{
					const FLevelActor& A = Session->GetActors()[ActorIndex];
					return FText::Format(LOCTEXT("ActorDecided", "{0}/{1}"), A.NumDecided(), A.NumItems());
				})
				.ColorAndOpacity_Lambda([this, ActorIndex]()
				{
					const FLevelActor& A = Session->GetActors()[ActorIndex];
					return A.NumDecided() == A.NumItems() ? DoneColor : Dim;
				})
			]
		];
}

void SGitLevelMergeWindow::OnActorSelected(TSharedPtr<FActorItem> Item, ESelectInfo::Type Info)
{
	if (!Item.IsValid() || Item->ActorIndex == INDEX_NONE)
		return;

	Selected = Item->ActorIndex;
	Session->Focus(Selected);
	RebuildDetails();
}

void SGitLevelMergeWindow::RebuildDetails()
{
	if (!Session->GetActors().IsValidIndex(Selected))
	{
		Details->SetContent(SNullWidget::NullWidget);
		return;
	}

	const FLevelActor& Actor = Session->GetActors()[Selected];
	Details->SetContent(Actor.bExistenceConflict ? MakeExistence(Selected) : MakeChanges(Selected));
}

TSharedRef<SWidget> SGitLevelMergeWindow::MakeChoice(TFunction<bool()> IsChosen, TFunction<void()> Choose, const FText& Label, const FText& Tip)
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.ToolTipText(Tip)
		.IsChecked_Lambda([IsChosen]() { return IsChosen() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([Choose](ECheckBoxState) { Choose(); })
		[
			SNew(SBox).Padding(FMargin(8, 2)).HAlign(HAlign_Center)
			[
				SNew(STextBlock).Text(Label)
			]
		];
}

TSharedRef<SWidget> SGitLevelMergeWindow::MakeBulkChoices(int32 ActorIndex, const FString& Category, bool bWithNeither)
{
	auto Pick = [this, ActorIndex, Category](ELevelChoice Choice)
	{
		return [this, ActorIndex, Category, Choice]() { Session->ChooseAll(ActorIndex, Category, Choice); };
	};
	auto Is = [this, ActorIndex, Category](ELevelChoice Choice)
	{
		return [this, ActorIndex, Category, Choice]() { return Session->IsAllChosen(ActorIndex, Category, Choice); };
	};

	TSharedRef<SHorizontalBox> Box = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			MakeChoice(Is(ELevelChoice::Ours), Pick(ELevelChoice::Ours), LOCTEXT("AllOurs", "Ours"), LOCTEXT("AllOursTip", "Use Ours for all of these"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
		[
			MakeChoice(Is(ELevelChoice::Theirs), Pick(ELevelChoice::Theirs), LOCTEXT("AllTheirs", "Theirs"), LOCTEXT("AllTheirsTip", "Use Theirs for all of these"))
		];

	if (bWithNeither)
	{
		Box->AddSlot().AutoWidth().Padding(4, 0, 0, 0)
		[
			MakeChoice(Is(ELevelChoice::Level), Pick(ELevelChoice::Level), LOCTEXT("AllNeither", "Neither"),
				LOCTEXT("AllNeitherTip", "Keep the level's current values for all of these (keep / delete decisions aren't changed)"))
		];
	}
	return Box;
}

TSharedRef<SWidget> SGitLevelMergeWindow::MakeExistence(int32 ActorIndex)
{
	const FLevelActor& Actor = Session->GetActors()[ActorIndex];
	const bool bTheirsDeleted = !Actor.bInTheirs;

	auto Pick = [this, ActorIndex](ELevelChoice Choice)
	{
		return [this, ActorIndex, Choice]() { Session->ChooseExistence(ActorIndex, Choice); };
	};
	auto Is = [this, ActorIndex](ELevelChoice Choice)
	{
		return [this, ActorIndex, Choice]() { return Session->GetActors()[ActorIndex].Existence == Choice; };
	};

	// What the side that kept the actor changed since base: shown so the choice is informed.
	TSharedRef<SVerticalBox> Kept = SNew(SVerticalBox);
	const FSlateColor KeptColor = bTheirsDeleted ? OursColor : TheirsColor;
	Kept->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.3f)[SNew(STextBlock).Text(LOCTEXT("ColProperty", "Property")).Font(FAppStyle::GetFontStyle("BoldFont"))]
		+ SHorizontalBox::Slot().FillWidth(0.35f)[SNew(STextBlock).Text(LOCTEXT("ColBase", "Base")).ColorAndOpacity(Dim).Font(FAppStyle::GetFontStyle("BoldFont"))]
		+ SHorizontalBox::Slot().FillWidth(0.35f)
		[
			SNew(STextBlock).ColorAndOpacity(KeptColor).Font(FAppStyle::GetFontStyle("BoldFont"))
			.Text(bTheirsDeleted ? LOCTEXT("ColOurs", "Ours") : LOCTEXT("ColTheirs", "Theirs"))
		]
	];
	for (const FLevelChange& Change : Actor.KeptSideChanges)
	{
		const FString Name = Change.Component.IsEmpty() ? Change.Name : Change.Component + TEXT(" > ") + Change.Name;
		Kept->AddSlot().AutoHeight().Padding(0, 3)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(0.3f).Padding(0, 0, 8, 0)[SNew(STextBlock).Text(FText::FromString(Name)).ToolTipText(FText::FromString(Change.Category))]
			+ SHorizontalBox::Slot().FillWidth(0.35f).Padding(0, 0, 8, 0)[ValueCell(Change.Base, Change.bInBase, Dim)]
			+ SHorizontalBox::Slot().FillWidth(0.35f)
			[
				ValueCell(bTheirsDeleted ? Change.Ours : Change.Theirs, bTheirsDeleted ? Change.bInOurs : Change.bInTheirs, KeptColor)
			]
		];
	}
	if (Actor.KeptSideChanges.Num() == 0)
		Kept->AddSlot().AutoHeight()[SNew(STextBlock).ColorAndOpacity(Dim).Text(LOCTEXT("KeptNothing", "No property changed."))];

	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::FromString(Actor.Label)).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(STextBlock).Text(FText::FromString(Actor.ClassName)).ColorAndOpacity(Dim)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
			[
				SNew(STextBlock)
				.Text(bTheirsDeleted ? LOCTEXT("ExistTheirs", "Deleted in Theirs, changed in Ours.") : LOCTEXT("ExistOurs", "Deleted in Ours, changed in Theirs."))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					MakeChoice(Is(ELevelChoice::Ours), Pick(ELevelChoice::Ours),
						bTheirsDeleted ? LOCTEXT("KeepOurs", "Keep (Ours)") : LOCTEXT("DeleteOurs", "Delete (Ours)"),
						LOCTEXT("OursTip", "Use what Ours did with this actor"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
				[
					MakeChoice(Is(ELevelChoice::Theirs), Pick(ELevelChoice::Theirs),
						bTheirsDeleted ? LOCTEXT("DeleteTheirs", "Delete (Theirs)") : LOCTEXT("KeepTheirs", "Keep (Theirs)"),
						LOCTEXT("TheirsTip", "Use what Theirs did with this actor"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SBox)
				.Visibility_Lambda([this, ActorIndex]()
				{
					return Session->GetActors()[ActorIndex].Result() == TEXT("deleted") && Session->GetActors()[ActorIndex].Existence != ELevelChoice::None
						? EVisibility::Visible : EVisibility::Collapsed;
				})
				[
					Warning(LOCTEXT("DeleteWarning", "Merge deletes this actor's file. Other actors that refer to it lose the reference."))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 18, 0, 6)
			[
				SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(bTheirsDeleted ? LOCTEXT("KeptOurs", "What Ours changed") : LOCTEXT("KeptTheirs", "What Theirs changed"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				Kept
			]
		];
}

TSharedRef<SWidget> SGitLevelMergeWindow::MakeChanges(int32 ActorIndex)
{
	const FLevelActor& Actor = Session->GetActors()[ActorIndex];
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);

	auto Header = [](const FText& Text, const FSlateColor& Color) -> TSharedRef<SWidget>
	{
		return SNew(STextBlock).Text(Text).ColorAndOpacity(Color).Font(FAppStyle::GetFontStyle("BoldFont"));
	};

	// Actor name and "this actor" choices
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 10)
	[
		Columns(
			{
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(FText::FromString(Actor.Label)).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(STextBlock).Text(FText::FromString(Actor.ClassName)).ColorAndOpacity(Dim)
				],
				SNullWidget::NullWidget, SNullWidget::NullWidget, SNullWidget::NullWidget, SNullWidget::NullWidget,
				SNew(STextBlock).Text(LOCTEXT("ThisActor", "This actor:")).Justification(ETextJustify::Right)
			},
			MakeBulkChoices(ActorIndex, FString(), true))
	];

	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		Columns(
			{
				Header(LOCTEXT("ColProperty", "Property"), FSlateColor::UseForeground()),
				Header(LOCTEXT("ColChange", "Change"), FSlateColor::UseForeground()),
				Header(LOCTEXT("ColBase", "Base"), Dim),
				Header(LOCTEXT("ColOurs", "Ours"), OursColor),
				Header(LOCTEXT("ColTheirs", "Theirs"), TheirsColor),
				SNew(STextBlock).Text(LOCTEXT("ColLevel", "In level")).Font(FAppStyle::GetFontStyle("BoldFont"))
					.ToolTipText(LOCTEXT("ColLevelTip", "The value in the level now; this is what Merge saves"))
			},
			Header(LOCTEXT("ColPick", "Use"), FSlateColor::UseForeground()))
	];

	FString Category;
	for (int32 ChangeIndex = 0; ChangeIndex < Actor.Changes.Num(); ++ChangeIndex)
	{
		const FLevelChange& Change = Actor.Changes[ChangeIndex];
		if (Change.Category != Category)
		{
			Category = Change.Category;
			Rows->AddSlot().AutoHeight().Padding(0, 8, 0, 2)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Header"))
				.Padding(FMargin(6, 3))
				[
					Columns(
						{
							SNew(STextBlock).Text(FText::FromString(Category)).Font(FAppStyle::GetFontStyle("BoldFont")),
							SNullWidget::NullWidget, SNullWidget::NullWidget, SNullWidget::NullWidget, SNullWidget::NullWidget, SNullWidget::NullWidget
						},
						MakeBulkChoices(ActorIndex, Category, true))
				]
			];
		}

		Rows->AddSlot().AutoHeight().Padding(6, 3, 6, 3)
		[
			MakeChangeRow(ActorIndex, ChangeIndex)
		];
	}

	if (Actor.Changes.Num() == 0)
		Rows->AddSlot().AutoHeight()[SNew(STextBlock).ColorAndOpacity(Dim).Text(LOCTEXT("NoChanges", "No property differs."))];

	return SNew(SScrollBox) + SScrollBox::Slot()[Rows];
}

TSharedRef<SWidget> SGitLevelMergeWindow::MakeChangeRow(int32 ActorIndex, int32 ChangeIndex)
{
	const FLevelActor& Actor = Session->GetActors()[ActorIndex];
	const FLevelChange& Change = Actor.Changes[ChangeIndex];

	auto Pick = [this, ActorIndex, ChangeIndex](ELevelChoice Choice)
	{
		return [this, ActorIndex, ChangeIndex, Choice]() { Session->Choose(ActorIndex, ChangeIndex, Choice); };
	};
	auto Is = [this, ActorIndex, ChangeIndex](ELevelChoice Choice)
	{
		return [this, ActorIndex, ChangeIndex, Choice]() { return Session->GetActors()[ActorIndex].Changes[ChangeIndex].Choice == Choice; };
	};

	FText KindLabel, KindTip;
	FSlateColor KindColor;
	KindText(Change.Kind, KindLabel, KindTip, KindColor);

	TSharedRef<SWidget> Property = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock).Text(FText::FromString(Change.Name)).ToolTipText(FText::FromString(Change.Key))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock).ColorAndOpacity(Dim).Font(FAppStyle::GetFontStyle("SmallFont"))
			.Visibility(Change.Component.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
			.Text(FText::FromString(Change.Component))
		];

	// The level's value now: amber when it was edited by hand after a side was chosen.
	TSharedRef<SWidget> InLevel = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
		[
			SNew(SImage).Image(FAppStyle::GetBrush("Icons.Warning")).ColorAndOpacity(WarnColor)
			.Visibility_Lambda([this, ActorIndex, ChangeIndex]() { return Session->IsEditedInLevel(ActorIndex, ChangeIndex) ? EVisibility::Visible : EVisibility::Collapsed; })
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text_Lambda([this, ActorIndex, ChangeIndex]()
			{
				const TOptional<FString> Value = Session->GetLiveValue(ActorIndex, ChangeIndex);
				return FText::FromString(Value.IsSet() ? Shorten(Value.GetValue()) : FString(TEXT("-")));
			})
			.ToolTipText_Lambda([this, ActorIndex, ChangeIndex]()
			{
				const TOptional<FString> Value = Session->GetLiveValue(ActorIndex, ChangeIndex);
				const FText Shown = FText::FromString(Value.IsSet() ? Value.GetValue() : FString(TEXT("-")));
				return Session->IsEditedInLevel(ActorIndex, ChangeIndex)
					? FText::Format(LOCTEXT("EditedTip", "{0}\n\nEdited in the level after you chose; this value is saved."), Shown)
					: Shown;
			})
			.ColorAndOpacity_Lambda([this, ActorIndex, ChangeIndex]()
			{
				return Session->IsEditedInLevel(ActorIndex, ChangeIndex) ? WarnColor : FSlateColor::UseForeground();
			})
		];

	TSharedRef<SWidget> Use = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			MakeChoice(Is(ELevelChoice::Ours), Pick(ELevelChoice::Ours), LOCTEXT("PickOurs", "Ours"), LOCTEXT("PickOursTip", "Use the Ours value"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
		[
			MakeChoice(Is(ELevelChoice::Theirs), Pick(ELevelChoice::Theirs), LOCTEXT("PickTheirs", "Theirs"), LOCTEXT("PickTheirsTip", "Use the Theirs value"))
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
		[
			MakeChoice(Is(ELevelChoice::Level), Pick(ELevelChoice::Level), LOCTEXT("PickNeither", "Neither"),
				LOCTEXT("PickNeitherTip", "Keep the value the level has now; you can edit it in Details"))
		];

	return Columns(
		{
			Property,
			SNew(STextBlock).Text(KindLabel).ToolTipText(KindTip).ColorAndOpacity(KindColor),
			ValueCell(Change.Base, Actor.bInBase && Change.bInBase, Dim),
			ValueCell(Change.Ours, Change.bInOurs, OursColor),
			ValueCell(Change.Theirs, Change.bInTheirs, TheirsColor),
			InLevel
		},
		Use);
}

FText SGitLevelMergeWindow::GetProgressText() const
{
	return FText::Format(LOCTEXT("Progress", "{0} of {1} decided"), Session->NumDecided(), Session->NumItems());
}

FText SGitLevelMergeWindow::GetWarningText() const
{
	const int32 Edited = Session->NumEditedInLevel();
	return Edited > 0 ? FText::Format(LOCTEXT("EditedWarning", "{0} value(s) edited in the level after choosing; the level's value is saved."), Edited) : FText::GetEmpty();
}

FReply SGitLevelMergeWindow::OnMerge()
{
	if (!Session->IsAllDecided())
		return FReply::Handled();

	// What Merge does, and anything that may surprise.
	FString Message = FString::Printf(TEXT("Merge level %s?\n\n- %d actor(s) saved"), *FPackageName::GetShortName(Session->GetLevel()), Session->NumResults(TEXT("saved")));
	if (const int32 Deleted = Session->NumResults(TEXT("deleted")))
		Message += FString::Printf(TEXT(", %d deleted"), Deleted);
	Message += TEXT(".");
	if (const int32 Neither = Session->NumChosen(ELevelChoice::Level))
		Message += FString::Printf(TEXT("\n- %d value(s) set to Neither keep the level's current value."), Neither);
	if (const int32 Edited = Session->NumEditedInLevel())
		Message += FString::Printf(TEXT("\n- %d value(s) were edited in the level after choosing; the level's value is saved."), Edited);
	if (const int32 Other = Session->NumOtherDirtyPackages())
		Message += FString::Printf(TEXT("\n- %d other unsaved change(s) in Unreal aren't part of the merge; Unreal asks about them when it closes."), Other);
	Message += TEXT("\n\nUnreal closes and GitO stages the result.");

	if (FMessageDialog::Open(EAppMsgType::OkCancel, FText::FromString(Message), LOCTEXT("MergeTitle", "Merge level")) != EAppReturnType::Ok)
		return FReply::Handled();

	if (Session->Save())
		OnDone.ExecuteIfBound();
	else
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("SaveFailed", "Unreal couldn't save the merged actors. See the Output Log."), LOCTEXT("MergeTitle", "Merge level"));
	return FReply::Handled();
}

FReply SGitLevelMergeWindow::OnCancel()
{
	// The window asks before closing when choices would be lost.
	OnDone.ExecuteIfBound();
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
