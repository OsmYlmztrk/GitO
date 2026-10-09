// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#pragma once

#include "BlueprintSnapshot.h"
#include "GraphEditor.h"
#include "Misc/NotifyHook.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class IDetailsView;
class IStructureDetailsView;
class SBox;
class SGraphEditor;
class UBlueprint;
class UDataTable;
class UEdGraphNode;
struct FTransactionContext;
enum class ETransactionStateEventType : uint8;

namespace GitBlueprintMerge
{
	class FMergeSession;
}

/**
 * Lets a person decide each conflict group (keep ours / take theirs / take both) while looking at both
 * versions and a live preview of the merged result in the middle, then merges and saves. Changes that
 * merge on their own are listed for reference.
 *
 * The result can also be edited by hand (graph, details, data table rows). Those edits go through the editor's
 * undo buffer: they can be undone one by one or all at once, and must be confirmed before Merge saves them.
 */
class SGitBlueprintMergeWindow : public SCompoundWidget, public FNotifyHook
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
	virtual ~SGitBlueprintMergeWindow() override;

	// FNotifyHook: data table rows edited in the result are recorded for undo.
	virtual void NotifyPreChange(FProperty* PropertyAboutToChange) override;
	virtual void NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged) override;

	/**
	 * True when the window may close now. With decisions made and nothing merged, it shows an inline
	 * "close without merging?" bar instead and returns false.
	 */
	bool ConfirmClose();

	/**
	 * Dragging nodes from ours/base/theirs into the result. Those graphs are read-only and capture the mouse, so a
	 * Slate input preprocessor watches the mouse for the whole app and calls these.
	 */
	void OnDragMouseDown(const FVector2D& ScreenPos);
	void OnDragMouseMove(const FVector2D& ScreenPos);
	void OnDragMouseUp(const FVector2D& ScreenPos);
	bool IsOverResultGraph(const FVector2D& ScreenPos) const;

	/** Graph shortcuts for the graph under the mouse (see MakeGraphCommands); true when one ran. */
	bool OnGraphKeyDown(const FKeyEvent& InKeyEvent);

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
	TSharedRef<SWidget> MakeGraphView(GitBlueprintMerge::EResolution Side, UBlueprint* Blueprint, const TArray<FString>& NodeIds);
	TSharedRef<SWidget> MakeSideView(GitBlueprintMerge::EResolution Side, const FRow* Row);
	TSharedRef<SWidget> MakeValuePanel(GitBlueprintMerge::EResolution Side, const FRow& Row) const;

	/**
	 * Unreal's own details panel for what a value row is about: class defaults, a component, a variable, Class
	 * Settings or a widget of a Blueprint; the asset itself for data assets and material instances; the row of a
	 * data table. Editable in the result column only.
	 */
	TSharedPtr<SWidget> MakeDetailsView(GitBlueprintMerge::EResolution Side, UObject* Asset, const FRow& Row);
	TSharedRef<IDetailsView> MakeObjectDetails(bool bEditable) const;

	/** Bar above the main area while the result has edits by hand: confirm, undo, revert. */
	TSharedRef<SWidget> MakeEditsBar();
	/** Bar asking whether a new choice may drop the edits by hand (the result is built again). */
	TSharedRef<SWidget> MakeDiscardBar();

	/** Edits by hand in the result that are not undone. */
	int32 NumEdits() const;
	bool HasEdits() const { return NumEdits() > 0; }
	void OnTransactionStateChanged(const FTransactionContext& Context, ETransactionStateEventType State);
	void UndoEdit();
	void RedoEdit();
	void AfterUndoRedo();
	void RevertEdits();
	void ConfirmEdits();
	/** Copies the nodes selected in a version's graph into the result; DropAt (graph coordinates) places them. */
	void CopySelectedToResult(GitBlueprintMerge::EResolution Side, const FVector2D* DropAt = nullptr);

	void DeleteSelectedInResult();
	void CopyNodesToResult(UBlueprint* Source, const TArray<UEdGraphNode*>& Nodes, const FVector2D* DropAt);

	/** Keyboard shortcuts of a graph (the only ones in the window): Ctrl+C everywhere; Ctrl+V, Delete, Ctrl+Z, Ctrl+Y in the result. */
	TSharedRef<class FUICommandList> MakeGraphCommands(GitBlueprintMerge::EResolution Side);
	void CopyToClipboard(GitBlueprintMerge::EResolution Side);
	void PasteFromClipboard();

	/**
	 * The node menu of the result graph (pin dropped on empty space, or right click): the same context-sensitive list as
	 * the Blueprint editor, limited to adding nodes; actions that change the Blueprint itself (promote to variable, ...) are left out.
	 */
	FActionMenuContent OnCreateResultActionMenu(UEdGraph* Graph, const FVector2f& Location, const TArray<UEdGraphPin*>& Pins, bool bAutoExpand, SGraphEditor::FActionMenuClosed OnClosed);
	/** Rebuilds the result column (after undo or a check); the others don't change. */
	void RefreshResult();

	/** Choices go through here: with edits by hand, the person is asked first, since the result is built again. */
	void SetChoices(const TMap<int32, GitBlueprintMerge::EResolution>& NewChoices, bool bAdvance);

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

	/** Graph editors on screen, by column, to copy or delete the nodes selected in them. */
	TMap<GitBlueprintMerge::EResolution, TWeakPtr<SGraphEditor>> GraphEditors;
	/** Details of the nodes selected in the result graph (editable). */
	TSharedPtr<IDetailsView> ResultNodeDetails;
	/** The data table row shown, editable, in the result column. */
	TWeakObjectPtr<UDataTable> ResultTable;
	FName ResultRowName;
	bool bInRowEdit = false;
	/** Data table row views on screen, by column; they must outlive their widgets. */
	TMap<GitBlueprintMerge::EResolution, TSharedPtr<IStructureDetailsView>> RowViews;

	/** Why the last edit action (copy, delete, confirm) didn't work; shown in the edits bar. */
	FString EditsMessage;
	bool bConfirmingRevert = false;

	/** Nodes copied with Ctrl+C (and the Blueprint they are in), pasted into the result with Ctrl+V. */
	TArray<TWeakObjectPtr<UEdGraphNode>> ClipboardNodes;
	TWeakObjectPtr<UBlueprint> ClipboardSource;
	/** Shortcut bindings of the graphs on screen, by column. */
	TMap<GitBlueprintMerge::EResolution, TSharedPtr<class FUICommandList>> GraphCommands;

	/** Node drag from a version into the result (see OnDragMouseDown). */
	TSharedPtr<class IInputProcessor> DragProcessor;
	TOptional<GitBlueprintMerge::EResolution> DragSide;
	FVector2D DragStart = FVector2D::ZeroVector;
	bool bDraggingNodes = false;
	bool bDropOverResult = false;

	/** The edits by hand were checked (compiled) and accepted; any new edit or undo clears it. */
	bool bEditsConfirmed = false;
	/** Our own changes to the preview (building, compiling) are not edits by hand. */
	bool bInternalChange = false;
	/** A choice waiting for "drop my edits?". */
	bool bConfirmingDiscard = false;
	bool bPendingAdvance = false;
	TMap<int32, GitBlueprintMerge::EResolution> PendingChoices;

	FString Problems;
	bool bShowBase = false;
	bool bConfirmingClose = false;
	bool bFinished = false;

	/** The current choices would take changes from theirs that can't be merged yet... */
	bool bNeedsLeaveOut = false;
	/** ...and the person agreed to leave them out and redo them by hand. */
	bool bLeaveOut = false;
};
