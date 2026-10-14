// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#include "Editor.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IMainFrameModule.h"
#include "LevelMerge.h"
#include "MergeSession.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"
#include "SGitBlueprintMergeWindow.h"
#include "SGitLevelMergeWindow.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "GitBlueprintMerge"

DEFINE_LOG_CATEGORY_STATIC(LogGitBlueprintMergeUI, Log, All);

/**
 * When the editor is started with -GitBlueprintMergeUI (by MyGitGUI), hides the main window, shows the
 * merge window for -Base/-Ours/-Theirs, writes -Report, saves the result to -Output and quits.
 * Otherwise the module does nothing.
 */
class FGitBlueprintMergeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// MyGitGUI level merges: -GitLevelMergeUI -Manifest=<json> -Report=<json>, with the level on the command line.
		bLevelMode = FParse::Param(FCommandLine::Get(), TEXT("GitLevelMergeUI"));
		if (IsRunningCommandlet() || (!bLevelMode && !FParse::Param(FCommandLine::Get(), TEXT("GitBlueprintMergeUI"))))
			return;

		IMainFrameModule& MainFrame = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		if (MainFrame.IsWindowInitialized())
			Open(MainFrame.GetParentWindow(), false);
		else
			MainFrame.OnMainFrameCreationFinished().AddRaw(this, &FGitBlueprintMergeModule::Open);
	}

	virtual void ShutdownModule() override
	{
		if (IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>("MainFrame"))
		{
			MainFrame->OnMainFrameCreationFinished().RemoveAll(this);
			if (CanCloseEditorHandle.IsValid())
				MainFrame->UnregisterCanCloseEditor(CanCloseEditorHandle);
		}
	}

private:
	void Open(TSharedPtr<SWindow> RootWindow, bool bIsRunningStartupDialog)
	{
		if (bIsRunningStartupDialog || Window.IsValid())
			return;

		if (bLevelMode)
		{
			OpenLevelMerge();
			return;
		}

		const TCHAR* CommandLine = FCommandLine::Get();
		FString BaseFile, OursFile, TheirsFile, OutputFile, PackageName, OursLabel, TheirsLabel;
		FParse::Value(CommandLine, TEXT("Base="), BaseFile);
		FParse::Value(CommandLine, TEXT("Ours="), OursFile);
		FParse::Value(CommandLine, TEXT("Theirs="), TheirsFile);
		FParse::Value(CommandLine, TEXT("Output="), OutputFile);
		FParse::Value(CommandLine, TEXT("PackageName="), PackageName);
		FParse::Value(CommandLine, TEXT("Report="), ReportFile);
		FParse::Value(CommandLine, TEXT("OursLabel="), OursLabel);
		FParse::Value(CommandLine, TEXT("TheirsLabel="), TheirsLabel);

		Session = MakeShared<GitBlueprintMerge::FMergeSession>();
		if (!Session->Load(BaseFile, OursFile, TheirsFile))
		{
			Quit();
			return;
		}

		// Look like a stand-alone tool: only the merge window is shown.
		if (RootWindow.IsValid())
			RootWindow->HideWindow();

		Window = SNew(SWindow)
			.Title(FText::Format(LOCTEXT("Title", "Merge {0} - MyGitGUI"), FText::FromString(Session->GetAssetName())))
			.ClientSize(FVector2D(1600.0, 900.0))
			.SupportsMaximize(true)
			.SupportsMinimize(true)
			[
				SAssignNew(MergeWidget, SGitBlueprintMergeWindow)
				.Session(Session)
				.OutputFile(OutputFile)
				.PackageName(PackageName)
				.OursLabel(OursLabel)
				.TheirsLabel(TheirsLabel)
				.OnDone_Raw(this, &FGitBlueprintMergeModule::Close)
			];

		// Closing with the X asks first when decisions would be lost.
		Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride::CreateRaw(this, &FGitBlueprintMergeModule::OnRequestClose));
		Window->SetOnWindowClosed(FOnWindowClosed::CreateRaw(this, &FGitBlueprintMergeModule::OnWindowClosed));
		FSlateApplication::Get().AddWindow(Window.ToSharedRef());


		UE_LOG(LogGitBlueprintMergeUI, Display, TEXT("Merge window opened for %s (%d conflict(s))."), *Session->GetAssetName(), Session->GetPlan().NumConflicts);
	}

	/**
	 * The level stays visible in the editor, so the user sees each actor while deciding. The editor loads the level
	 * from the command line around the time the main frame appears; the window waits for that, so the live actors
	 * it finds aren't replaced by a second load, then opens on top of the editor.
	 */
	void OpenLevelMerge()
	{
		FParse::Value(FCommandLine::Get(), TEXT("Manifest="), ManifestFile);
		FParse::Value(FCommandLine::Get(), TEXT("Report="), ReportFile);
		WaitedFor = 0.0f;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FGitBlueprintMergeModule::TryOpenLevelMerge), 0.5f);

		// Closing the editor during a level merge asks first, and the merged actors aren't offered for saving.
		IMainFrameModule& MainFrame = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		CanCloseEditorHandle = MainFrame.RegisterCanCloseEditor(IMainFrameModule::FMainFrameCanCloseEditor::CreateRaw(this, &FGitBlueprintMergeModule::CanCloseEditor));
	}

	bool CanCloseEditor()
	{
		if (bQuitting || !LevelSession.IsValid() || LevelSession->WasWritten())
			return true;
		if (LevelWidget.IsValid() && !LevelWidget->ConfirmClose())
			return false;

		LevelSession->Discard();
		return true;
	}

	bool TryOpenLevelMerge(float DeltaTime)
	{
		WaitedFor += DeltaTime;
		const FString Level = GitBlueprintMerge::FLevelMergeSession::ReadManifestLevel(ManifestFile);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const bool bReady = World && World->GetPackage()->GetName() == Level && !IsAsyncLoading() && !GIsSlowTask;
		if (!bReady && WaitedFor < 120.0f)
			return true;

		LevelSession = MakeShared<GitBlueprintMerge::FLevelMergeSession>();
		if (!LevelSession->Load(ManifestFile))
		{
			Quit();
			return false;
		}

		Window = SNew(SWindow)
			.Title(FText::Format(LOCTEXT("LevelTitle", "Merge level {0} - GitO"), FText::FromString(FPackageName::GetShortName(LevelSession->GetLevel()))))
			.ClientSize(FVector2D(1100.0, 600.0))
			.SupportsMaximize(true)
			.SupportsMinimize(true)
			[
				SAssignNew(LevelWidget, SGitLevelMergeWindow)
				.Session(LevelSession)
				.OnDone_Raw(this, &FGitBlueprintMergeModule::Close)
			];

		// Cancel and the X ask first when choices would be lost.
		Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride::CreateRaw(this, &FGitBlueprintMergeModule::OnRequestClose));
		Window->SetOnWindowClosed(FOnWindowClosed::CreateRaw(this, &FGitBlueprintMergeModule::OnWindowClosed));

		// A child of the editor's main window stays on top of it.
		IMainFrameModule& MainFrame = FModuleManager::LoadModuleChecked<IMainFrameModule>("MainFrame");
		if (TSharedPtr<SWindow> Parent = MainFrame.GetParentWindow())
			FSlateApplication::Get().AddWindowAsNativeChild(Window.ToSharedRef(), Parent.ToSharedRef());
		else
			FSlateApplication::Get().AddWindow(Window.ToSharedRef());
		Window->BringToFront(true);
		return false;
	}

	void Close()
	{
		if (Window.IsValid())
			FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
	}

	void OnRequestClose(const TSharedRef<SWindow>& InWindow)
	{
		if (MergeWidget.IsValid() && !MergeWidget->ConfirmClose())
			return;
		if (LevelWidget.IsValid() && !LevelWidget->ConfirmClose())
			return;

		InWindow->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride());
		FSlateApplication::Get().RequestDestroyWindow(InWindow);
	}

	void OnWindowClosed(const TSharedRef<SWindow>&)
	{
		Quit();
	}

	void Quit()
	{
		// The report tells MyGitGUI whether a result was written (cancel = not written).
		if (Session.IsValid() && !ReportFile.IsEmpty())
			Session->WriteReport(ReportFile);

		bQuitting = true;
		if (LevelSession.IsValid())
		{
			if (!LevelSession->WasWritten())
				LevelSession->Discard();
			if (!ReportFile.IsEmpty())
				LevelSession->WriteReport(ReportFile);
			Window.Reset();
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			return;
		}

		UE_LOG(LogGitBlueprintMergeUI, Display, TEXT("Merge window closed (%s)."), Session.IsValid() && Session->WasWritten() ? TEXT("merged") : TEXT("not merged"));

		GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
	}

	TSharedPtr<GitBlueprintMerge::FMergeSession> Session;
	TSharedPtr<GitBlueprintMerge::FLevelMergeSession> LevelSession;
	bool bLevelMode = false;
	FString ManifestFile;
	float WaitedFor = 0.0f;
	TSharedPtr<SWindow> Window;
	TSharedPtr<SGitBlueprintMergeWindow> MergeWidget;
	TSharedPtr<SGitLevelMergeWindow> LevelWidget;
	FDelegateHandle CanCloseEditorHandle;
	bool bQuitting = false;
	FString ReportFile;
};

IMPLEMENT_MODULE(FGitBlueprintMergeModule, GitBlueprintMerge)

#undef LOCTEXT_NAMESPACE
