#include "pch.h"
#include "MainPage.xaml.h"

using namespace RuffleUWP;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

App::App()
{
    InitializeComponent();
    Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
    Resuming += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &App::OnResuming);
    // Exception chưa bắt (XAML/C++/CX): hiện lên màn hình thay vì tắt app
    UnhandledException += ref new UnhandledExceptionEventHandler([](Platform::Object^, UnhandledExceptionEventArgs^ e) {
        e->Handled = true;
        wchar_t hr[32]; swprintf_s(hr, L" (0x%08X)", (unsigned)e->Exception.Value);
        Platform::String^ msg = "Unhandled: " + e->Message + ref new Platform::String(hr);
        auto frame = dynamic_cast<Frame^>(Window::Current ? Window::Current->Content : nullptr);
        if (auto page = frame ? dynamic_cast<MainPage^>(frame->Content) : nullptr) page->ShowError(msg);
        else DebugLogW((std::wstring(msg->Data()) + L"\n").c_str());
    });
}

void App::EnsureWindow()
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr)
        frame->Navigate(Windows::UI::Xaml::Interop::TypeName(MainPage::typeid));
    Window::Current->Activate();
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    DebugLog("OnLaunched\n");
    try { EnsureWindow(); }
    catch (Platform::Exception^ ex) { DebugLogW((L"EnsureWindow exception: " + std::wstring(ex->Message->Data()) + L"\n").c_str()); throw; }
    DebugLog("Window activated\n");
}

void App::OnFileActivated(FileActivatedEventArgs^ e)
{
    EnsureWindow();
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    auto page = dynamic_cast<MainPage^>(frame->Content);
    if (page && e->Files->Size > 0)
    {
        auto file = dynamic_cast<Windows::Storage::StorageFile^>(e->Files->GetAt(0));
        if (file) page->OpenFile(file);
    }
}

static MainPage^ CurrentPage()
{
    auto frame = dynamic_cast<Frame^>(Window::Current ? Window::Current->Content : nullptr);
    return frame ? dynamic_cast<MainPage^>(frame->Content) : nullptr;
}

void App::OnSuspending(Platform::Object^ sender, SuspendingEventArgs^ e)
{
    if (auto page = CurrentPage()) page->SetSuspended(true);
    DebugLogCopyToDocuments();
}

void App::OnResuming(Platform::Object^ sender, Platform::Object^ e)
{
    if (auto page = CurrentPage()) page->SetSuspended(false);
}
