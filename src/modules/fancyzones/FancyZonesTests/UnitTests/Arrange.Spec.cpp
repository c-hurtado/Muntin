#include "pch.h"

#include <filesystem>
#include <fstream>

#include <FancyZonesLib/Arrange.h>
#include <FancyZonesLib/Settings.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace FancyZonesUnitTests
{
    TEST_CLASS (ArrangeUnitTests)
    {
        TEST_METHOD_CLEANUP(Cleanup)
        {
            // Runs after a failed assert too: back to defaults, whatever the test left. The file is the unit tests' own (test-settings.json).
            FancyZonesSettings::instance().SetSettings(Settings{});
            std::filesystem::remove(FancyZonesSettings::GetSettingsFileName());
        }

        TEST_METHOD (PropertyHandsOffOnlyWhileTheProcessIsAlive)
        {
            const auto alive = [](DWORD pid) { return pid == 42; };
            Assert::IsTrue(Arrange::PropertyHandsOff(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(42)), alive));
            Assert::IsFalse(Arrange::PropertyHandsOff(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(43)), alive), L"stale pid");
            Assert::IsFalse(Arrange::PropertyHandsOff(nullptr, [](DWORD) { return true; }), L"no pid");
        }

        TEST_METHOD (RealProcessCheck)
        {
            Assert::IsFalse(Arrange::ProcessIsAlive(GetCurrentProcessId()), L"alive, but not a PowerShell image");
            Assert::IsFalse(Arrange::ProcessIsAlive(0));
            Assert::IsFalse(Arrange::ProcessIsAlive(0xFFFFFFF0)); // No such process
        }

        TEST_METHOD (ImageBasenameRule)
        {
            Assert::IsTrue(Arrange::IsStageProtoImage(LR"(C:\Program Files\PowerShell\7\pwsh.exe)"));
            Assert::IsTrue(Arrange::IsStageProtoImage(LR"(C:\Windows\System32\WindowsPowerShell\v1.0\PowerShell.EXE)"));
            Assert::IsTrue(Arrange::IsStageProtoImage(LR"(pwsh.exe)"));
            Assert::IsFalse(Arrange::IsStageProtoImage(LR"(C:\x\notpwsh.exe)"));
            Assert::IsFalse(Arrange::IsStageProtoImage(LR"(C:\pwsh.exe\evil.exe)"));
            Assert::IsFalse(Arrange::IsStageProtoImage(LR"()"));
        }

        TEST_METHOD (IsStageFollowsTheSetting)
        {
            Settings stage;
            stage.arrangeMode = 1;
            FancyZonesSettings::instance().SetSettings(stage);
            Assert::IsTrue(Arrange::IsStage());
            Settings dock;
            dock.arrangeMode = 0;
            FancyZonesSettings::instance().SetSettings(dock);
            Assert::IsFalse(Arrange::IsStage());
        }

        TEST_METHOD (FreshInstallIsStageWithStrip)
        {
            Assert::AreEqual(1, Settings{}.arrangeMode, L"Stage mode");
            Assert::AreEqual(std::wstring(L"stage"), Settings{}.stageLayout, L"the stage layout");
            Assert::AreEqual(1, Settings{}.stagePromoteStyle, L"promote crossfades");
        }

        TEST_METHOD (FirstLoadWithoutAFileIsStageAndWritesItDown)
        {
            const auto file = FancyZonesSettings::GetSettingsFileName();
            std::filesystem::remove(file);
            Settings dock;
            dock.arrangeMode = 0;
            dock.stageLayout = L"pair";
            FancyZonesSettings::instance().SetSettings(dock);
            FancyZonesSettings::instance().ForgetLoaded();
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(1, FancyZonesSettings::settings().arrangeMode);
            Assert::AreEqual(std::wstring(L"stage"), FancyZonesSettings::settings().stageLayout);

            // Written down: a file another setting makes later still says Stage
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::DockGapID, json::value(5), file));
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(1, FancyZonesSettings::settings().arrangeMode);
            Assert::AreEqual(std::wstring(L"stage"), FancyZonesSettings::settings().stageLayout);
        }

        TEST_METHOD (FirstLoadOfAnUnreadableFileIsDock)
        {
            const auto file = FancyZonesSettings::GetSettingsFileName();
            {
                std::wofstream out(file);
                out << L"{ not json";
            }

            FancyZonesSettings::instance().SetSettings(Settings{});
            FancyZonesSettings::instance().ForgetLoaded();
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(0, FancyZonesSettings::settings().arrangeMode);
            Assert::AreEqual(std::wstring(L"master"), FancyZonesSettings::settings().stageLayout);

            // Later, a file that can't be read (an editor saving it) changes nothing
            Settings stage;
            FancyZonesSettings::instance().SetSettings(stage);
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(1, FancyZonesSettings::settings().arrangeMode);
        }

        TEST_METHOD (AFileWithoutTheKeysIsDockAndMaster)
        {
            const auto file = FancyZonesSettings::GetSettingsFileName();
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::DockGapID, json::value(5), file));
            FancyZonesSettings::instance().SetSettings(Settings{});
            FancyZonesSettings::instance().ForgetLoaded();
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(0, FancyZonesSettings::settings().arrangeMode);
            Assert::AreEqual(std::wstring(L"master"), FancyZonesSettings::settings().stageLayout);

            // Stage without a layout: master, as before
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::ArrangeModeID, json::value(1), file));
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(1, FancyZonesSettings::settings().arrangeMode);
            Assert::AreEqual(std::wstring(L"master"), FancyZonesSettings::settings().stageLayout);

            Assert::AreEqual(1, ArrangeFallback::For(false).arrangeMode);
            Assert::AreEqual(std::wstring(L"stage"), std::wstring(ArrangeFallback::For(false).stageLayout));
        }

        TEST_METHOD (SaveValuesWritesTheModeAndLayoutAtOnce)
        {
            const auto file = FancyZonesSettings::GetSettingsFileName();
            Settings dock;
            dock.arrangeMode = 0;
            dock.stageLayout = L"master";
            FancyZonesSettings::instance().SetSettings(dock);
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::DockGapID, json::value(5), file));
            Assert::IsTrue(FancyZonesSettings::SaveValues({ { NonLocalizable::StageLayoutID, json::value(L"pair") }, { NonLocalizable::ArrangeModeID, json::value(1) } }, file));
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(1, FancyZonesSettings::settings().arrangeMode);
            Assert::AreEqual(std::wstring(L"pair"), FancyZonesSettings::settings().stageLayout);
            Assert::AreEqual(5, FancyZonesSettings::settings().dockGap, L"the other keys stay");

            // Dock leaves the layout as it is
            Assert::IsTrue(FancyZonesSettings::SaveValues({ { NonLocalizable::ArrangeModeID, json::value(0) } }, file));
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(0, FancyZonesSettings::settings().arrangeMode);
            Assert::AreEqual(std::wstring(L"pair"), FancyZonesSettings::settings().stageLayout);
        }

        TEST_METHOD (SaveValuesKeepsAnUnreadableFile)
        {
            const auto file = FancyZonesSettings::GetSettingsFileName();
            {
                std::wofstream out(file);
                out << L"{ not json";
            }

            Assert::IsFalse(FancyZonesSettings::SaveValues({ { NonLocalizable::ArrangeModeID, json::value(0) } }, file));
            std::wifstream in(file);
            std::wstring text((std::istreambuf_iterator<wchar_t>(in)), std::istreambuf_iterator<wchar_t>());
            Assert::AreEqual(std::wstring(L"{ not json"), text);
        }

        TEST_METHOD (CombineRules)
        {
            Assert::IsFalse(Arrange::Combine(false, false, false, false));
            Assert::IsTrue(Arrange::Combine(true, false, false, false));
            Assert::IsFalse(Arrange::Combine(false, false, true, false), L"held counts in stage mode only");
            Assert::IsTrue(Arrange::Combine(false, true, true, false));
            Assert::IsTrue(Arrange::Combine(false, false, false, true));
        }

        TEST_METHOD (NoPropertyAndStaleOwnerDoNotHandOff)
        {
            Assert::IsFalse(Arrange::HandsOff(nullptr));
            const HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr); // Never shown
            Assert::IsNotNull(window);
            Assert::IsFalse(Arrange::HandsOff(window));
            SetPropW(window, Arrange::HandsOffProperty, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GetCurrentProcessId())));
            Assert::IsFalse(Arrange::HandsOff(window), L"a live process that is not a PowerShell image");
            SetPropW(window, Arrange::HandsOffProperty, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(0xFFFFFFF0)));
            Assert::IsFalse(Arrange::HandsOff(window), L"a dead owner doesn't hand off");
            RemovePropW(window, Arrange::HandsOffProperty);
            DestroyWindow(window);
        }

        TEST_METHOD (StageSettingsRoundTrip)
        {
            const auto file = FancyZonesSettings::GetSettingsFileName();
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::ArrangeModeID, json::value(1), file));
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StageLayoutID, json::value(L"master"), file));
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StagePairCutID, json::value(420), file));
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StageMasterCutID, json::value(9999), file));
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StageRowsID, json::value(L"500,300,200"), file));
            FancyZonesSettings::instance().LoadSettings();

            const auto& s = FancyZonesSettings::settings();
            Assert::AreEqual(1, s.arrangeMode);
            Assert::IsTrue(Arrange::IsStage());
            Assert::AreEqual(std::wstring(L"master"), s.stageLayout);
            Assert::AreEqual(420, s.stagePairCut);
            Assert::AreEqual(850, s.stageMasterCut, L"clamped");
            Assert::AreEqual(std::wstring(L"500,300,200"), s.stageRows);

            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StageLayoutID, json::value(L"bogus"), file));
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::ArrangeModeID, json::value(0), file));
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(std::wstring(L"master"), FancyZonesSettings::settings().stageLayout, L"unknown layout ignored");
            Assert::IsFalse(Arrange::IsStage());
        }

        TEST_METHOD (RowsParseAndFormat)
        {
            const auto rows = Arrange::ParseRows(L"500,300,200");
            Assert::AreEqual(size_t{ 3 }, rows.size());
            Assert::AreEqual(0.3, rows[1], 1e-9);
            Assert::AreEqual(std::wstring(L"500,300,200"), Arrange::FormatRows(rows));
            Assert::IsTrue(Arrange::ParseRows(L"").empty());
            Assert::IsTrue(Arrange::ParseRows(L"500,,200").empty());
            Assert::IsTrue(Arrange::ParseRows(L"5x0").empty());
            Assert::IsTrue(Arrange::ParseRows(L"0,500").empty());
        }
    };
}
