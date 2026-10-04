// Copyright (c) MyGitGUI contributors. MIT License.

#pragma once

#include "BlueprintSnapshot.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SBox;
class UBlueprint;

namespace GitBlueprintMerge
{
	class FMergeSession;
}

/**
 * Lets a person decide each conflict group (keep ours / take theirs / take both) while looking at both
 * versions and a live preview of the merged result in the middle, then merges and saves. Changes that
 * merge on their own are listed for reference.
 *
 * Keys: O keep ours, T take theirs, A take both, N / P next / previous conflict, B show base, Ctrl+1..4 layout,
 * Ctrl+Enter merge, Esc cancel.
 */
class SGitBlueprintMergeWindow : public SCompoundWidget
{
public:
	DECLARE_DELEGATE(FOnDone);

	SLATE_BEGIN_ARGS(SGitBlueprintMergeWindow) {}
		SLATE_ARGUMENT(TSharedPtr<GitBlueprintMerge::FMergeSession>, Session)
		SLATE_ARGUMENT(FString, OutputFile)
		SLATE_ARGUMENT(FString, PackageName)
		SLATE_ARGUMENT(FString, OursLabel)
		SLATE_ARGUMENT(FString, TheirsLabel)
		SLATE_EVENT(FOnDone, OnDone)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * True when the window may close now. With decisions made and nothing merged, it shows an inline
	 * "close without merging?" bar instead and returns false.
	 */
	bool ConfirmClose();

	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	enum class ERowKind : uint8
	{
		Conflict,
		Change,
	};

	struct FRow
	{
		ERowKind Kind = ERowKind::Change;
		int32 Group = INDEX_NONE;
		FString Title;
		FString Ours;
		FString Theirs;
		FString Details;
		GitBlueprintMerge::EResolution Resolution = GitBlueprintMerge::EResolution::Unchanged;
		TArray<FString> NodeIds;
		/** Plan decisions that aren't about nodes (variables, class defaults, ...), shown as values above the graphs. */
		TArray<int32> Values;
		/** Both sides' nodes can be kept, one after the other. */
		bool bCanTakeBoth = false;
	};

	void BuildRows();

	/** Layouts of the main area: 0 changes on top, 1 side list, 2 classic three-way, 3 result focus. */
	static constexpr int32 NumLayouts = 4;
	void SetLayout(int32 Index);
	void RebuildLayout();
	TSharedRef<SWidget> MakeLayout(int32 Index);
	TSharedRef<SWidget> MakeLayoutSwitcher();
	TSharedRef<SWidget> MakeChangesPanel();
	TSharedRef<SWidget> MakeChangesSplitter();
	TSharedRef<SWidget> MakeBaseColumn();

	TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FRow> Row, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<SWidget> MakeSectionHeader(const FText& Label, const FText& Tip) const;
	TSharedRef<SWidget> MakeConflictRow(const TSharedPtr<FRow>& Row);
	TSharedRef<SWidget> MakeChangeRow(const TSharedPtr<FRow>& Row);
	TSharedRef<SWidget> MakeSideLine(int32 Group, GitBlueprintMerge::EResolution Side, const FString& Text);
	TSharedRef<SWidget> MakeChoiceButton(int32 Group, GitBlueprintMerge::EResolution Side);
	TSharedRef<SWidget> MakeGraphColumn(GitBlueprintMerge::EResolution Side, TSharedPtr<SBox>& OutBox);
	TSharedRef<SWidget> MakeGraphView(GitBlueprintMerge::EResolution Side, UBlueprint* Blueprint, const TArray<FString>& NodeIds) const;
	TSharedRef<SWidget> MakeSideView(GitBlueprintMerge::EResolution Side, const FRow* Row) const;
	TSharedRef<SWidget> MakeValuePanel(GitBlueprintMerge::EResolution Side, const FRow& Row) const;

	/**
	 * Unreal's own (read-only) details panel for what a value row is about: class defaults or a component of a
	 * Blueprint, or the asset itself for data assets and material instances.
	 */
	TSharedPtr<SWidget> MakeDetailsView(UObject* Asset, const FRow& Row) const;

	void OnSelectionChanged(TSharedPtr<FRow> Row, ESelectInfo::Type SelectInfo);
	void ShowRow(const FRow* Row);
	void Choose(int32 Group, GitBlueprintMerge::EResolution Side);
	void ChooseAll(GitBlueprintMerge::EResolution Side);
	bool SelectConflict(int32 Direction, bool bOnlyOpen);
	int32 GetSelectedGroup() const;
	const FRow* GetSelectedRow() const;
	bool IsChosen(int32 Group, GitBlueprintMerge::EResolution Side) const;
	/** The choice keeps this side's version: chosen alone, or as part of "take both". */
	bool Takes(int32 Group, GitBlueprintMerge::EResolution Side) const;
	bool IsBoth(int32 Group) const;
	TSharedRef<SWidget> MakeOrderToggle(int32 Group);
	FText GetPreviewStatus() const;
	FText SideName(GitBlueprintMerge::EResolution Side) const;

	int32 NumGroups() const;
	FText GetProgressText() const;
	bool AllDecided() const;
	bool CanMerge() const;
	void OnChoicesChanged();
	FReply OnMerge();
	FReply OnCancel();

	TSharedPtr<GitBlueprintMerge::FMergeSession> Session;
	FString OutputFile;
	FString PackageName;
	FString OursLabel;
	FString TheirsLabel;
	FOnDone OnDone;

	TArray<TSharedPtr<FRow>> ConflictRows;
	TArray<TSharedPtr<FRow>> ChangeRows;
	TMap<int32, GitBlueprintMerge::EResolution> Choices;

	/** Conflicts, as cards side by side. */
	TSharedPtr<SListView<TSharedPtr<FRow>>> List;
	/** What merged on its own. */
	TSharedPtr<SListView<TSharedPtr<FRow>>> ChangeList;
	/** The row shown in the graphs, from either list. */
	TSharedPtr<FRow> SelectedRow;
	TSharedPtr<SBox> MainArea;
	int32 LayoutIndex = 0;
	bool bCardsSideBySide = true;
	TSharedPtr<SBox> OursBox;
	TSharedPtr<SBox> TheirsBox;
	TSharedPtr<SBox> BaseBox;
	TSharedPtr<SBox> ResultBox;

	FString Problems;
	bool bShowBase = false;
	bool bConfirmingClose = false;
	bool bFinished = false;

	/** The current choices would take changes from theirs that can't be merged yet... */
	bool bNeedsLeaveOut = false;
	/** ...and the person agreed to leave them out and redo them by hand. */
	bool bLeaveOut = false;
};
