// Copyright (c) 2026 Osman Yılmaztürk. MIT License.

#include "Editor.h"
#include "Engine/Engine.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IMainFrameModule.h"
#include "MergeSession.h"
#include "Misc/CommandLine.h"
#include "Modules/ModuleManager.h"
#include "SGitBlueprintMergeWindow.h"
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
		if (IsRunningCommandlet() || !FParse::Param(FCommandLine::Get(), TEXT("GitBlueprintMergeUI")))
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
			MainFrame->OnMainFrameCreationFinished().RemoveAll(this);
	}

private:
	void Open(TSharedPtr<SWindow> RootWindow, bool bIsRunningStartupDialog)
	{
		if (bIsRunningStartupDialog || Window.IsValid())
			return;

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

	void Close()
	{
		if (Window.IsValid())
			FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
	}

	void OnRequestClose(const TSharedRef<SWindow>& InWindow)
	{
		if (MergeWidget.IsValid() && !MergeWidget->ConfirmClose())
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

		UE_LOG(LogGitBlueprintMergeUI, Display, TEXT("Merge window closed (%s)."), Session.IsValid() && Session->WasWritten() ? TEXT("merged") : TEXT("not merged"));

		GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
	}

	TSharedPtr<GitBlueprintMerge::FMergeSession> Session;
	TSharedPtr<SWindow> Window;
	TSharedPtr<SGitBlueprintMergeWindow> MergeWidget;
	FString ReportFile;
};

IMPLEMENT_MODULE(FGitBlueprintMergeModule, GitBlueprintMerge)

#undef LOCTEXT_NAMESPACE
