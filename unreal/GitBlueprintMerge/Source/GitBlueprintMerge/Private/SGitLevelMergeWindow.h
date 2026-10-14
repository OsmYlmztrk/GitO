// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "LevelMerge.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SBox;

DECLARE_DELEGATE(FOnLevelMergeDone);

/**
 * Level merge window: the conflicted actors of one level on the left (changed on both sides, deleted on one side),
 * the selected actor's changed properties on the right, grouped by Details category, with Ours / Theirs / Neither
 * for each, for a category, an actor or the whole level. Nothing is chosen for the user; Merge stays off until
 * everything is decided. Buttons only (no keyboard shortcuts).
 */
class SGitLevelMergeWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGitLevelMergeWindow) {}
		SLATE_ARGUMENT(TSharedPtr<GitBlueprintMerge::FLevelMergeSession>, Session)
		SLATE_EVENT(FOnLevelMergeDone, OnDone)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** False when the user doesn't want to drop their choices. */
	bool ConfirmClose() const;

private:
	/** A row of the actor list: an actor, or a section header. */
	struct FActorItem
	{
		int32 ActorIndex = INDEX_NONE;
		FText Header;
	};

	TSharedRef<ITableRow> MakeActorRow(TSharedPtr<FActorItem> Item, const TSharedRef<STableViewBase>& Owner);
	void OnActorSelected(TSharedPtr<FActorItem> Item, ESelectInfo::Type Info);
	void RebuildDetails();
	TSharedRef<SWidget> MakeExistence(int32 ActorIndex);
	TSharedRef<SWidget> MakeChanges(int32 ActorIndex);
	TSharedRef<SWidget> MakeChangeRow(int32 ActorIndex, int32 ChangeIndex);
	TSharedRef<SWidget> MakeChoice(TFunction<bool()> IsChosen, TFunction<void()> Choose, const FText& Label, const FText& Tip);

	/** Ours / Theirs / Neither for the items of one actor (or all), one category (or all). */
	TSharedRef<SWidget> MakeBulkChoices(int32 ActorIndex, const FString& Category, bool bWithNeither);

	FText GetProgressText() const;
	FText GetWarningText() const;
	FReply OnMerge();
	FReply OnCancel();

	TSharedPtr<GitBlueprintMerge::FLevelMergeSession> Session;
	FOnLevelMergeDone OnDone;
	TArray<TSharedPtr<FActorItem>> ActorItems;
	TSharedPtr<SListView<TSharedPtr<FActorItem>>> ActorList;
	TSharedPtr<SBox> Details;
	int32 Selected = INDEX_NONE;
};
