#include "Modules/ModuleManager.h"

#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/PlatformApplicationMisc.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/ConfigCacheIni.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWeakWidget.h"
#include "Widgets/Text/STextBlock.h"

namespace ViewfinderPauseMenu
{
    constexpr float MinSensitivity = 0.01f;
    constexpr float MaxSensitivity = 0.20f;
    constexpr float DefaultSensitivity = 0.07f;
    constexpr float MinFOV = 60.0f;
    constexpr float MaxFOV = 120.0f;
    constexpr float DefaultFOV = 90.0f;
    const TCHAR* SettingsSection = TEXT("ViewfinderPauseMenu.Settings");
}

class FViewfinderPauseMenuModule;

class SViewfinderPauseMenu final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SViewfinderPauseMenu) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, FViewfinderPauseMenuModule* InOwner);

private:
    FReply HandleContinue();
    FReply HandleRestart();
    FReply HandleQuit();
    void HandleSensitivityChanged(float NormalizedValue);
    void HandleFOVChanged(float NormalizedValue);
    FText GetSensitivityText() const;
    FText GetFOVText() const;

    FViewfinderPauseMenuModule* Owner = nullptr;
    float Sensitivity = ViewfinderPauseMenu::DefaultSensitivity;
    float FOV = ViewfinderPauseMenu::DefaultFOV;
};

class FViewfinderPauseMenuInputProcessor final : public IInputProcessor
{
public:
    explicit FViewfinderPauseMenuInputProcessor(FViewfinderPauseMenuModule* InOwner)
        : Owner(InOwner)
    {
    }

    virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override
    {
    }

    virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;

    virtual const TCHAR* GetDebugName() const override
    {
        return TEXT("ViewfinderPauseMenuInputProcessor");
    }

private:
    FViewfinderPauseMenuModule* Owner = nullptr;
};

class FViewfinderPauseMenuModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        LoadSettings();
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateRaw(this, &FViewfinderPauseMenuModule::Tick));
    }

    virtual void ShutdownModule() override
    {
        if (TickHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
            TickHandle.Reset();
        }

        if (FSlateApplication::IsInitialized() && InputProcessor.IsValid())
        {
            FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
        }

        RemoveMenuFromViewport();
        InputProcessor.Reset();
    }

    bool ToggleMenu()
    {
        if (!GetGameWorld())
        {
            return false;
        }

        if (bMenuVisible)
        {
            HideMenu();
        }
        else
        {
            ShowMenu();
        }
        return true;
    }

    void ShowMenu()
    {
        UWorld* World = GetGameWorld();
        APlayerController* PlayerController = GetPlayerController();
        if (!World || !PlayerController || !GEngine || !GEngine->GameViewport)
        {
            return;
        }

        ApplySettings(PlayerController);

        SAssignNew(MenuWidget, SViewfinderPauseMenu, this);
        ViewportContent = SNew(SWeakWidget).PossiblyNullContent(MenuWidget.ToSharedRef());
        GEngine->GameViewport->AddViewportWidgetContent(ViewportContent.ToSharedRef(), 10000);

        bMenuVisible = true;
        UGameplayStatics::SetGamePaused(World, true);
        PlayerController->bShowMouseCursor = true;

        FInputModeUIOnly InputMode;
        InputMode.SetWidgetToFocus(MenuWidget);
        InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        PlayerController->SetInputMode(InputMode);

        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().SetKeyboardFocus(MenuWidget, EFocusCause::SetDirectly);
        }
    }

    void HideMenu()
    {
        UWorld* World = GetGameWorld();
        APlayerController* PlayerController = GetPlayerController();

        RemoveMenuFromViewport();
        bMenuVisible = false;

        if (World)
        {
            UGameplayStatics::SetGamePaused(World, false);
        }

        if (PlayerController)
        {
            PlayerController->bShowMouseCursor = false;
            FInputModeGameOnly InputMode;
            InputMode.SetConsumeCaptureMouseDown(false);
            PlayerController->SetInputMode(InputMode);
            PlayerController->FlushPressedKeys();
        }

        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().SetAllUserFocusToGameViewport(EFocusCause::SetDirectly);
        }
    }

    void RestartLevel()
    {
        UWorld* World = GetGameWorld();
        if (!World)
        {
            return;
        }

        const FString LevelName = UGameplayStatics::GetCurrentLevelName(World, true);
        HideMenu();
        UGameplayStatics::OpenLevel(World, FName(*LevelName));
    }

    void QuitGame()
    {
        UWorld* World = GetGameWorld();
        APlayerController* PlayerController = GetPlayerController();
        if (!World || !PlayerController)
        {
            return;
        }

        UKismetSystemLibrary::QuitGame(World, PlayerController, EQuitPreference::Quit, false);
    }

    float GetSensitivity() const
    {
        return Sensitivity;
    }

    float GetFOV() const
    {
        return FOV;
    }

    void SetSensitivity(float NewSensitivity)
    {
        Sensitivity = FMath::Clamp(NewSensitivity, ViewfinderPauseMenu::MinSensitivity, ViewfinderPauseMenu::MaxSensitivity);
        ApplySensitivity(GetPlayerController());
        SaveSettings();
    }

    void SetFOV(float NewFOV)
    {
        FOV = FMath::Clamp(NewFOV, ViewfinderPauseMenu::MinFOV, ViewfinderPauseMenu::MaxFOV);
        ApplyFOV(GetPlayerController());
        SaveSettings();
    }

private:
    bool Tick(float DeltaTime)
    {
        if (!InputProcessor.IsValid() && FSlateApplication::IsInitialized())
        {
            InputProcessor = MakeShared<FViewfinderPauseMenuInputProcessor>(this);
            FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor, 0);
        }

        APlayerController* PlayerController = GetPlayerController();
        if (PlayerController && LastConfiguredPlayerController.Get() != PlayerController)
        {
            ApplySettings(PlayerController);
            LastConfiguredPlayerController = PlayerController;
        }
        else if (!PlayerController)
        {
            LastConfiguredPlayerController.Reset();
            if (bMenuVisible)
            {
                RemoveMenuFromViewport();
                bMenuVisible = false;
            }
        }

        return true;
    }

    UWorld* GetGameWorld() const
    {
        if (!GEngine)
        {
            return nullptr;
        }

        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game)
            {
                if (UWorld* World = Context.World())
                {
                    return World;
                }
            }
        }

        return nullptr;
    }

    APlayerController* GetPlayerController() const
    {
        if (UWorld* World = GetGameWorld())
        {
            return UGameplayStatics::GetPlayerController(World, 0);
        }
        return nullptr;
    }

    void LoadSettings()
    {
        Sensitivity = ViewfinderPauseMenu::DefaultSensitivity;
        FOV = ViewfinderPauseMenu::DefaultFOV;

        if (GConfig)
        {
            GConfig->GetFloat(ViewfinderPauseMenu::SettingsSection, TEXT("MouseSensitivity"), Sensitivity, GGameUserSettingsIni);
            GConfig->GetFloat(ViewfinderPauseMenu::SettingsSection, TEXT("FieldOfView"), FOV, GGameUserSettingsIni);
        }

        Sensitivity = FMath::Clamp(Sensitivity, ViewfinderPauseMenu::MinSensitivity, ViewfinderPauseMenu::MaxSensitivity);
        FOV = FMath::Clamp(FOV, ViewfinderPauseMenu::MinFOV, ViewfinderPauseMenu::MaxFOV);
    }

    void SaveSettings() const
    {
        if (!GConfig)
        {
            return;
        }

        GConfig->SetFloat(ViewfinderPauseMenu::SettingsSection, TEXT("MouseSensitivity"), Sensitivity, GGameUserSettingsIni);
        GConfig->SetFloat(ViewfinderPauseMenu::SettingsSection, TEXT("FieldOfView"), FOV, GGameUserSettingsIni);
        GConfig->Flush(false, GGameUserSettingsIni);
    }

    void ApplySettings(APlayerController* PlayerController) const
    {
        ApplySensitivity(PlayerController);
        ApplyFOV(PlayerController);
    }

    void ApplySensitivity(APlayerController* PlayerController) const
    {
        if (!PlayerController || !PlayerController->PlayerInput)
        {
            return;
        }

        const FKey MouseKeys[] = {EKeys::MouseX, EKeys::MouseY, EKeys::Mouse2D};
        for (const FKey& MouseKey : MouseKeys)
        {
            FInputAxisProperties AxisProperties;
            if (PlayerController->PlayerInput->GetAxisProperties(MouseKey, AxisProperties))
            {
                AxisProperties.Sensitivity = Sensitivity;
                PlayerController->PlayerInput->SetAxisProperties(MouseKey, AxisProperties);
            }
        }
    }

    void ApplyFOV(APlayerController* PlayerController) const
    {
        if (!PlayerController)
        {
            return;
        }

        if (APawn* Pawn = PlayerController->GetPawn())
        {
            if (UCameraComponent* Camera = Pawn->FindComponentByClass<UCameraComponent>())
            {
                Camera->SetFieldOfView(FOV);
            }
        }

        if (APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager)
        {
            CameraManager->DefaultFOV = FOV;
            CameraManager->UnlockFOV();
        }
    }

    void RemoveMenuFromViewport()
    {
        if (ViewportContent.IsValid() && GEngine && GEngine->GameViewport)
        {
            GEngine->GameViewport->RemoveViewportWidgetContent(ViewportContent.ToSharedRef());
        }

        ViewportContent.Reset();
        MenuWidget.Reset();
    }

    bool bMenuVisible = false;
    float Sensitivity = ViewfinderPauseMenu::DefaultSensitivity;
    float FOV = ViewfinderPauseMenu::DefaultFOV;
    FTSTicker::FDelegateHandle TickHandle;
    TSharedPtr<FViewfinderPauseMenuInputProcessor> InputProcessor;
    TSharedPtr<SViewfinderPauseMenu> MenuWidget;
    TSharedPtr<SWidget> ViewportContent;
    TWeakObjectPtr<APlayerController> LastConfiguredPlayerController;

    friend class SViewfinderPauseMenu;
};

bool FViewfinderPauseMenuInputProcessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
    if (Owner && InKeyEvent.GetKey() == EKeys::Escape && !InKeyEvent.IsRepeat())
    {
        return Owner->ToggleMenu();
    }
    return false;
}

void SViewfinderPauseMenu::Construct(const FArguments& InArgs, FViewfinderPauseMenuModule* InOwner)
{
    Owner = InOwner;
    Sensitivity = Owner ? Owner->GetSensitivity() : ViewfinderPauseMenu::DefaultSensitivity;
    FOV = Owner ? Owner->GetFOV() : ViewfinderPauseMenu::DefaultFOV;

    const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 34);
    const FSlateFontInfo LabelFont = FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 18);
    const FSlateFontInfo ButtonFont = FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 18);

    ChildSlot
    [
        SNew(SOverlay)

        + SOverlay::Slot()
        [
            SNew(SBorder)
            .BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f))
        ]

        + SOverlay::Slot()
        .HAlign(HAlign_Center)
        .VAlign(VAlign_Center)
        [
            SNew(SBox)
            .WidthOverride(520.0f)
            [
                SNew(SBorder)
                .Padding(FMargin(42.0f, 34.0f))
                .BorderBackgroundColor(FLinearColor(0.035f, 0.045f, 0.065f, 0.98f))
                [
                    SNew(SVerticalBox)

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .HAlign(HAlign_Center)
                    .Padding(0.0f, 0.0f, 0.0f, 24.0f)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(TEXT("PAUSED")))
                        .Font(TitleFont)
                        .ColorAndOpacity(FLinearColor::White)
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(TEXT("Mouse Sensitivity")))
                            .Font(LabelFont)
                            .ColorAndOpacity(FLinearColor(0.88f, 0.91f, 1.0f, 1.0f))
                        ]
                        + SHorizontalBox::Slot().AutoWidth()
                        [
                            SNew(STextBlock)
                            .Text(this, &SViewfinderPauseMenu::GetSensitivityText)
                            .Font(LabelFont)
                            .ColorAndOpacity(FLinearColor(0.40f, 0.76f, 1.0f, 1.0f))
                        ]
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 22.0f)
                    [
                        SNew(SSlider)
                        .Value((Sensitivity - ViewfinderPauseMenu::MinSensitivity) /
                            (ViewfinderPauseMenu::MaxSensitivity - ViewfinderPauseMenu::MinSensitivity))
                        .OnValueChanged(this, &SViewfinderPauseMenu::HandleSensitivityChanged)
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 8.0f)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1.0f)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(TEXT("Field of View")))
                            .Font(LabelFont)
                            .ColorAndOpacity(FLinearColor(0.88f, 0.91f, 1.0f, 1.0f))
                        ]
                        + SHorizontalBox::Slot().AutoWidth()
                        [
                            SNew(STextBlock)
                            .Text(this, &SViewfinderPauseMenu::GetFOVText)
                            .Font(LabelFont)
                            .ColorAndOpacity(FLinearColor(0.40f, 0.76f, 1.0f, 1.0f))
                        ]
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 26.0f)
                    [
                        SNew(SSlider)
                        .Value((FOV - ViewfinderPauseMenu::MinFOV) /
                            (ViewfinderPauseMenu::MaxFOV - ViewfinderPauseMenu::MinFOV))
                        .OnValueChanged(this, &SViewfinderPauseMenu::HandleFOVChanged)
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 18.0f)
                    [
                        SNew(SSeparator)
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 10.0f)
                    [
                        SNew(SButton)
                        .ContentPadding(FMargin(18.0f, 11.0f))
                        .HAlign(HAlign_Center)
                        .OnClicked(this, &SViewfinderPauseMenu::HandleContinue)
                        [
                            SNew(STextBlock).Text(FText::FromString(TEXT("Continue"))).Font(ButtonFont)
                        ]
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 10.0f)
                    [
                        SNew(SButton)
                        .ContentPadding(FMargin(18.0f, 11.0f))
                        .HAlign(HAlign_Center)
                        .OnClicked(this, &SViewfinderPauseMenu::HandleRestart)
                        [
                            SNew(STextBlock).Text(FText::FromString(TEXT("Restart"))).Font(ButtonFont)
                        ]
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        SNew(SButton)
                        .ContentPadding(FMargin(18.0f, 11.0f))
                        .HAlign(HAlign_Center)
                        .OnClicked(this, &SViewfinderPauseMenu::HandleQuit)
                        [
                            SNew(STextBlock).Text(FText::FromString(TEXT("Quit Game"))).Font(ButtonFont)
                        ]
                    ]
                ]
            ]
        ]
    ];
}

FReply SViewfinderPauseMenu::HandleContinue()
{
    if (Owner)
    {
        Owner->HideMenu();
    }
    return FReply::Handled();
}

FReply SViewfinderPauseMenu::HandleRestart()
{
    if (Owner)
    {
        Owner->RestartLevel();
    }
    return FReply::Handled();
}

FReply SViewfinderPauseMenu::HandleQuit()
{
    if (Owner)
    {
        Owner->QuitGame();
    }
    return FReply::Handled();
}

void SViewfinderPauseMenu::HandleSensitivityChanged(float NormalizedValue)
{
    Sensitivity = FMath::Lerp(ViewfinderPauseMenu::MinSensitivity, ViewfinderPauseMenu::MaxSensitivity, NormalizedValue);
    if (Owner)
    {
        Owner->SetSensitivity(Sensitivity);
    }
}

void SViewfinderPauseMenu::HandleFOVChanged(float NormalizedValue)
{
    FOV = FMath::Lerp(ViewfinderPauseMenu::MinFOV, ViewfinderPauseMenu::MaxFOV, NormalizedValue);
    if (Owner)
    {
        Owner->SetFOV(FOV);
    }
}

FText SViewfinderPauseMenu::GetSensitivityText() const
{
    FNumberFormattingOptions NumberFormat;
    NumberFormat.SetMinimumFractionalDigits(2);
    NumberFormat.SetMaximumFractionalDigits(2);
    return FText::AsNumber(Sensitivity, &NumberFormat);
}

FText SViewfinderPauseMenu::GetFOVText() const
{
    return FText::AsNumber(FMath::RoundToInt(FOV));
}

IMPLEMENT_MODULE(FViewfinderPauseMenuModule, ViewfinderPauseMenu)
