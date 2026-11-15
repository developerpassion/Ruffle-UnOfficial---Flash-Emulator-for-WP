#include "pch.h"
#include "MainPage.xaml.h"

using namespace RuffleUWP;
using namespace concurrency;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::Storage::Streams;
using namespace Windows::System;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;

MainPage::MainPage()
{
    InitializeComponent();
#ifdef RUFFLE_SAFE_MODE
    // Build chẩn đoán (msbuild /p:SafeMode=true): chỉ hiện XAML, không D3D/audio/Rust
    return;
#endif
    try
    {
        m_player = std::make_unique<RufflePlayer>(swapChainPanel);
        if (FAILED(m_player->AudioStatus()))
        {
            wchar_t msg[64]; swprintf_s(msg, L"Audio (XAudio2) init failed: 0x%08X", (unsigned)m_player->AudioStatus());
            ShowError(ref new Platform::String(msg));
        }
        // Settings: nạp LocalState\settings.json, áp dụng trước khi mở game nào
        m_fpsTimer = ref new DispatcherTimer();
        Windows::Foundation::TimeSpan fpsTick; fpsTick.Duration = 5000000LL; // 0.5 giây
        m_fpsTimer->Interval = fpsTick;
        m_fpsTimer->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(
            [this](Platform::Object^, Platform::Object^) { UpdateFpsText(); });
        m_settings = AppSettings::Load();
        ApplySettings();
        SyncSettingsUi();
        settingsFlyout->Opening += ref new Windows::Foundation::EventHandler<Platform::Object^>(
            [this](Platform::Object^, Platform::Object^) { SyncSettingsUi(); });
        {
            // Phiên bản lấy từ Package.appxmanifest (Identity Version)
            auto v = Windows::ApplicationModel::Package::Current->Id->Version;
            wchar_t buf[64];
            swprintf_s(buf, L"Version %u.%u.%u.%u", v.Major, v.Minor, v.Build, v.Revision);
            versionText->Text = ref new Platform::String(buf);
        }
#if defined(RUFFLE_STOP_STAGE) && RUFFLE_STOP_STAGE < 7
        return; // build chẩn đoán: không nối sự kiện / Resize
#endif
    }
    catch (Platform::Exception^ ex)
    {
        wchar_t hr[32]; swprintf_s(hr, L" (0x%08X)", (unsigned)ex->HResult);
        ShowError("Player init failed: " + ex->Message + ref new Platform::String(hr));
        return;
    }

    swapChainPanel->PointerPressed      += ref new PointerEventHandler([this](Platform::Object^, PointerRoutedEventArgs^ e) { HandlePointerPressed(e); });
    swapChainPanel->PointerMoved        += ref new PointerEventHandler([this](Platform::Object^, PointerRoutedEventArgs^ e) { HandlePointerMoved(e); });
    swapChainPanel->PointerReleased     += ref new PointerEventHandler([this](Platform::Object^, PointerRoutedEventArgs^ e) { HandlePointerReleased(e); });
    swapChainPanel->PointerCanceled     += ref new PointerEventHandler([this](Platform::Object^, PointerRoutedEventArgs^ e) { HandlePointerLost(e); });
    swapChainPanel->PointerCaptureLost  += ref new PointerEventHandler([this](Platform::Object^, PointerRoutedEventArgs^ e) { HandlePointerLost(e); });
    swapChainPanel->PointerWheelChanged += ref new PointerEventHandler([this](Platform::Object^, PointerRoutedEventArgs^ e) {
        m_player->MouseWheel(e->GetCurrentPoint(swapChainPanel)->Properties->MouseWheelDelta / 120.0);
        e->Handled = true;
    });

    m_longPress = ref new DispatcherTimer();
    Windows::Foundation::TimeSpan ts; ts.Duration = 8LL * 10000000LL; // 8 giây (đơn vị 100ns)
    m_longPress->Interval = ts;
    m_longPress->Tick += ref new Windows::Foundation::EventHandler<Platform::Object^>(this, &MainPage::OnLongPress);
    swapChainPanel->SizeChanged += ref new SizeChangedEventHandler([this](Platform::Object^, SizeChangedEventArgs^ e) {
        FitSettingsHeight(); // xoay máy khi popup Settings đang mở
        try { m_player->Resize(e->NewSize.Width, e->NewSize.Height); }
        catch (Platform::Exception^ ex)
        {
            wchar_t hr[32]; swprintf_s(hr, L" (0x%08X)", (unsigned)ex->HResult);
            ShowError("Resize/swapchain failed: " + ex->Message + ref new Platform::String(hr));
        }
    });
    // Đổi DPI/zoom (vd. đổi màn hình, Continuum) -> tạo lại buffer theo scale mới
    swapChainPanel->CompositionScaleChanged += ref new Windows::Foundation::TypedEventHandler<SwapChainPanel^, Platform::Object^>(
        [this](SwapChainPanel^ p, Platform::Object^) { m_player->Resize((float)p->ActualWidth, (float)p->ActualHeight); });
#if defined(RUFFLE_STOP_STAGE) && RUFFLE_STOP_STAGE == 7
    return; // build chẩn đoán: có Resize/swapchain, chưa có bàn phím/Back/Loaded
#endif

    // Bàn phím vật lý (CoreWindow nhận trước khi XAML định tuyến)
    auto window = CoreWindow::GetForCurrentThread();
    window->KeyDown += ref new Windows::Foundation::TypedEventHandler<CoreWindow^, KeyEventArgs^>(
        [this](CoreWindow^, KeyEventArgs^ e) { OnKey(e, true); });
    window->KeyUp += ref new Windows::Foundation::TypedEventHandler<CoreWindow^, KeyEventArgs^>(
        [this](CoreWindow^, KeyEventArgs^ e) { OnKey(e, false); });
    window->CharacterReceived += ref new Windows::Foundation::TypedEventHandler<CoreWindow^, CharacterReceivedEventArgs^>(
        [this](CoreWindow^, CharacterReceivedEventArgs^ e) {
            if (m_sinkFocused) return; // khi TextBox ẩn có focus, ký tự đi qua TextChanged
            m_player->CharEvent(e->KeyCode);
        });

    // Bàn phím ảo: text gõ vào TextBox ẩn -> chuyển từng ký tự cho Ruffle rồi xoá
    keyboardSink->GotFocus  += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) { m_sinkFocused = true; });
    keyboardSink->LostFocus += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        m_sinkFocused = false;
        keyboardSink->IsTabStop = false; // không cho Windows tự trả focus về đây (vd. sau hộp chọn file) -> bàn phím tự bật
    });
    keyboardSink->TextChanged += ref new TextChangedEventHandler([this](Platform::Object^, TextChangedEventArgs^) {
        auto text = keyboardSink->Text;
        if (text->IsEmpty()) return;
        const wchar_t* s = text->Data();
        for (unsigned i = 0; i < text->Length(); ++i)
        {
            uint32_t cp = s[i];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text->Length())
                cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
            if (cp == '\r' || cp == '\n') continue; // Enter đã đi qua KeyDown
            m_player->CharEvent(cp);
        }
        keyboardSink->Text = ""; // gây thêm 1 TextChanged rỗng, bị bỏ qua ở trên
    });
#if defined(RUFFLE_STOP_STAGE) && RUFFLE_STOP_STAGE == 8
    return; // build chẩn đoán: đã có bàn phím, chưa có Back/Loaded
#endif

    Loaded += ref new RoutedEventHandler([this](Platform::Object^, RoutedEventArgs^) {
        // Nút Back phần cứng (lần 1 -> Escape cho SWF): TẮT mặc định. Trên W10M 10586 (Lumia 650) đăng ký
        // SystemNavigationManager.BackRequested làm app chết ngay khi mở (cả trong constructor lẫn ở Loaded,
        // try/catch không bắt được). Nút Back giữ hành vi mặc định của hệ thống (thoát app).
        // Bật lại để thử: msbuild /p:EnableBack=true
#ifdef RUFFLE_ENABLE_BACK
        if (!m_backHooked)
        {
            m_backHooked = true;
            try
            {
                SystemNavigationManager::GetForCurrentView()->BackRequested +=
                    ref new Windows::Foundation::EventHandler<BackRequestedEventArgs^>(
                        [this](Platform::Object^ s, BackRequestedEventArgs^ e) { OnBack(s, e); });
            }
            catch (Platform::Exception^) {}
        }
#endif
        // Debug/test (chỉ khi có LocalState\debug.flag): tự load autoload.swf, dump frame sau ~2s ra frame.bmp
        if (!DebugLogEnabled()) return;
        auto folder = ApplicationData::Current->LocalFolder;
        create_task(folder->TryGetItemAsync("autoload.swf")).then([this, folder](IStorageItem^ item) {
            auto file = dynamic_cast<StorageFile^>(item);
            if (file)
            {
                m_player->RequestFrameDump(std::wstring(folder->Path->Data()) + L"\\frame.bmp");
                OpenFile(file);
            }
        }, task_continuation_context::use_current());
    });
}

uint32_t MainPage::Modifiers()
{
    auto w = CoreWindow::GetForCurrentThread();
    uint32_t m = 0;
    if ((w->GetKeyState(VirtualKey::Shift) & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down) m |= 1;
    if ((w->GetKeyState(VirtualKey::Control) & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down) m |= 2;
    return m;
}

void MainPage::OnKey(KeyEventArgs^ e, bool down)
{
    if (e->VirtualKey == VirtualKey::GoBack) return; // xử lý ở BackRequested
    m_player->KeyEvent((uint32_t)e->VirtualKey, Modifiers(), down);
}

void MainPage::OnBack(Platform::Object^, BackRequestedEventArgs^ e)
{
    ULONGLONG now = GetTickCount64();
    if (now - m_lastBack < 1500) return; // không Handled -> hệ thống thoát app
    m_lastBack = now;
    e->Handled = true;
    m_player->KeyEvent((uint32_t)VirtualKey::Escape, 0, true);
    m_player->KeyEvent((uint32_t)VirtualKey::Escape, 0, false);
}

void MainPage::OnKeyboardClick(Platform::Object^, RoutedEventArgs^)
{
    if (m_sinkFocused)
        this->Focus(Windows::UI::Xaml::FocusState::Programmatic); // ẩn bàn phím ảo
    else
    {
        keyboardSink->IsTabStop = true; // chỉ nhận focus khi người dùng bấm nút Keyboard
        keyboardSink->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    }
    if (auto bar = dynamic_cast<CommandBar^>(BottomAppBar)) bar->IsOpen = false;
}

void MainPage::SetSuspended(bool suspended)
{
    m_player->SetSuspended(suspended);
}

using Windows::UI::Input::PointerUpdateKind;
using Windows::Devices::Input::PointerDeviceType;

static bool IsTouch(PointerRoutedEventArgs^ e) { return e->Pointer->PointerDeviceType == PointerDeviceType::Touch; }

void MainPage::HandlePointerPressed(PointerRoutedEventArgs^ e)
{
    if (m_menuOpen) return;
    auto pt = e->GetCurrentPoint(swapChainPanel);
    swapChainPanel->CapturePointer(e->Pointer); // vẫn nhận "nhả" khi kéo ra ngoài vùng vẽ
    if (IsTouch(e))
    {
        m_touches[e->Pointer->PointerId] = pt->Position;
        if (m_pinching || m_touches.size() >= 2)
        {
            if (!m_pinching) BeginPinch(); // ngón thứ 2 chạm -> bắt đầu zoom
            e->Handled = true;
            return;
        }
        m_lastTouch = pt->Position;
    }
    m_player->PointerEvent(0, pt->Position.X, pt->Position.Y);
    HandleButtonUpdate(pt->Properties->PointerUpdateKind, pt->Position, IsTouch(e));
    e->Handled = true;
}

void MainPage::HandlePointerMoved(PointerRoutedEventArgs^ e)
{
    if (m_menuOpen) return;
    auto pt = e->GetCurrentPoint(swapChainPanel);
    auto pos = pt->Position;
    if (IsTouch(e))
    {
        auto it = m_touches.find(e->Pointer->PointerId);
        if (it != m_touches.end()) it->second = pos;
        if (m_pinching)
        {
            UpdatePinch();
            return;
        }
        m_lastTouch = pos;
    }
    if (m_touchActive && IsTouch(e) && m_longPress->IsEnabled)
    {
        float dx = pos.X - m_touchStart.X, dy = pos.Y - m_touchStart.Y;
        if (dx * dx + dy * dy > 20.0f * 20.0f) m_longPress->Stop(); // kéo đi -> không phải "giữ"
    }
    m_player->PointerEvent(0, pos.X, pos.Y);
    // Chuột: bấm thêm nút khi đã giữ nút khác chỉ đến qua PointerMoved
    HandleButtonUpdate(pt->Properties->PointerUpdateKind, pos, IsTouch(e));
}

void MainPage::HandlePointerReleased(PointerRoutedEventArgs^ e)
{
    auto pt = e->GetCurrentPoint(swapChainPanel);
    if (IsTouch(e))
    {
        m_touches.erase(e->Pointer->PointerId);
        if (m_pinching)
        {
            if (m_touches.empty()) m_pinching = false; // nhấc hết ngón -> kết thúc zoom
            else if (m_touches.size() >= 2) BeginPinch(); // còn 2+ ngón: tính lại tâm/khoảng cách
            swapChainPanel->ReleasePointerCapture(e->Pointer);
            e->Handled = true;
            return;
        }
    }
    if (!m_menuOpen)
        HandleButtonUpdate(pt->Properties->PointerUpdateKind, pt->Position, IsTouch(e));
    else if (IsTouch(e)) { m_touchActive = false; m_longPress->Stop(); }
    swapChainPanel->ReleasePointerCapture(e->Pointer);
    e->Handled = true;
}

void MainPage::HandlePointerLost(PointerRoutedEventArgs^ e)
{
    // Hệ thống huỷ pointer (cuộc gọi đến, mở app bar...) -> nhả nút trái để không bị kẹt
    if (IsTouch(e))
    {
        m_touches.erase(e->Pointer->PointerId);
        if (m_pinching)
        {
            if (m_touches.empty()) m_pinching = false;
            return;
        }
    }
    if (IsTouch(e) && m_touchActive)
    {
        m_longPress->Stop();
        m_touchActive = false;
        if (!m_suppressTouchUp)
        {
            auto pos = e->GetCurrentPoint(swapChainPanel)->Position;
            m_player->MouseButton(2, 0, pos.X, pos.Y);
        }
    }
}

void MainPage::HandleButtonUpdate(PointerUpdateKind kind, Windows::Foundation::Point pos, bool isTouch)
{
    switch (kind)
    {
    case PointerUpdateKind::LeftButtonPressed:
        if (isTouch)
        {
            m_touchActive = true;
            m_suppressTouchUp = false;
            m_touchStart = pos;
            m_longPress->Stop();
            m_longPress->Start();
        }
        m_player->MouseButton(1, 0, pos.X, pos.Y);
        break;
    case PointerUpdateKind::LeftButtonReleased:
        if (isTouch)
        {
            m_longPress->Stop();
            m_touchActive = false;
            if (m_suppressTouchUp) { m_suppressTouchUp = false; break; }
        }
        m_player->MouseButton(2, 0, pos.X, pos.Y);
        break;
    case PointerUpdateKind::RightButtonPressed:
        // Giống desktop Ruffle: nếu SWF không nhận chuột phải thì mở context menu
        if (!m_player->MouseButton(1, 1, pos.X, pos.Y)) ShowContextMenu(pos);
        break;
    case PointerUpdateKind::RightButtonReleased:
        m_player->MouseButton(2, 1, pos.X, pos.Y);
        break;
    case PointerUpdateKind::MiddleButtonPressed:
        m_player->MouseButton(1, 2, pos.X, pos.Y);
        break;
    case PointerUpdateKind::MiddleButtonReleased:
        m_player->MouseButton(2, 2, pos.X, pos.Y);
        break;
    default:
        break;
    }
}

void MainPage::OnLongPress(Platform::Object^, Platform::Object^)
{
    m_longPress->Stop();
    if (!m_touchActive || m_menuOpen) return;
    auto pos = m_touchStart;
    // Nhả chuột trái trước (tránh kẹt nút), rồi giả lập chuột phải như desktop
    m_suppressTouchUp = true;
    m_player->MouseButton(2, 0, pos.X, pos.Y);
    if (!m_player->MouseButton(1, 1, pos.X, pos.Y)) ShowContextMenu(pos);
    else m_player->MouseButton(2, 1, pos.X, pos.Y);
}

void MainPage::ShowContextMenu(Windows::Foundation::Point pos)
{
    auto items = m_player->PrepareContextMenu();
    if (items.empty())
    {
        m_player->ContextMenuClosed(pos.X, pos.Y); // vẫn gửi MouseUp phải
        return;
    }
    auto flyout = ref new MenuFlyout();
    for (uint32_t i = 0; i < items.size(); ++i)
    {
        const auto& it = items[i];
        if (it.separatorBefore && i > 0) flyout->Items->Append(ref new MenuFlyoutSeparator());
        MenuFlyoutItem^ mi;
        if (it.checked)
        {
            auto t = ref new ToggleMenuFlyoutItem();
            t->IsChecked = true;
            mi = t;
        }
        else mi = ref new MenuFlyoutItem();
        mi->Text = ref new Platform::String(it.caption.c_str());
        mi->IsEnabled = it.enabled;
        mi->Click += ref new RoutedEventHandler([this, i](Platform::Object^, RoutedEventArgs^) {
            m_player->ContextMenuSelect(i);
        });
        flyout->Items->Append(mi);
    }
    flyout->Closed += ref new Windows::Foundation::EventHandler<Platform::Object^>([this, pos](Platform::Object^, Platform::Object^) {
        m_menuOpen = false;
        m_player->ContextMenuClosed(pos.X, pos.Y);
    });
    // ShowAt(element, point) chỉ có từ 1607; dùng điểm neo để chạy được trên 10586
    menuAnchor->Margin = Thickness(pos.X, pos.Y, 0, 0);
    m_menuOpen = true;
    flyout->ShowAt(menuAnchor);
}

void MainPage::OnOpenClick(Platform::Object^, RoutedEventArgs^)
{
    auto picker = ref new FileOpenPicker();
    picker->FileTypeFilter->Append(".swf");
    create_task(picker->PickSingleFileAsync()).then([this](StorageFile^ file) {
        if (file) OpenFile(file);
    }, task_continuation_context::use_current());
}

void MainPage::OpenFile(StorageFile^ file)
{
    create_task(FileIO::ReadBufferAsync(file)).then([this](IBuffer^ buf) {
        auto reader = DataReader::FromBuffer(buf);
        std::vector<uint8_t> data(buf->Length);
        if (!data.empty())
            reader->ReadBytes(Platform::ArrayReference<uint8_t>(data.data(), (unsigned)data.size()));
        bool ok = !data.empty() && m_player->Load(std::move(data));
        // Ẩn màn hình chào khi đã nạp được game; lỗi (file không phải SWF) -> giữ nút để chọn lại
        welcomePanel->Visibility = ok ? Windows::UI::Xaml::Visibility::Collapsed : Windows::UI::Xaml::Visibility::Visible;
    }, task_continuation_context::use_current());
}

// ---------------------------------------------------------------- Settings (LocalState\settings.json)

static const float kScales[] = { 0.0f, 1.0f, 0.75f, 0.5f }; // scaleCombo: Auto / 100% / 75% / 50%
static const int kLimits[] = { 0, 30, 20 };                 // limitCombo: Unlimited / 30 / 20 FPS

// Áp dụng m_settings vào player + giao diện (gọi lúc mở app và mỗi lần đổi)
void MainPage::ApplySettings()
{
    if (!m_player) return;
    m_player->SetAudioEnabled(m_settings.audio);                      // dừng/chạy engine XAudio2, không nạp lại game
    m_player->SetUseGpu(m_settings.gpu && m_player->GpuAvailable()); // đổi -> nạp lại SWF đang chạy
    bool msaa = m_settings.msaa < 0 ? m_player->MsaaDefault() : m_settings.msaa == 1;
    m_player->SetMsaa(msaa);
    if (m_player->RenderScaleSetting() != m_settings.renderScale) m_player->SetRenderScale(m_settings.renderScale);
    m_player->SetQuality(m_settings.quality);
    m_player->SetFrameLimit(m_settings.frameLimit);
    fpsText->Visibility = m_settings.showFps ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    if (m_settings.showFps) m_fpsTimer->Start(); else m_fpsTimer->Stop();
}

// Ô chọn Render resolution / Quality / Frame limit
void MainPage::OnSettingComboChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^)
{
    if (m_syncingUi || !m_player) return;
    // Phải ép sender về ComboBox^ trước khi so (Object^ và ComboBox^ là hai interface COM khác nhau,
    // so trực tiếp luôn sai -> giá trị vừa chọn không được ghi nhận)
    auto cb = dynamic_cast<ComboBox^>(sender);
    if (!cb || cb->SelectedIndex < 0) return;
    if (cb == scaleCombo) m_settings.renderScale = kScales[cb->SelectedIndex];
    else if (cb == qualityCombo) m_settings.quality = cb->SelectedIndex;
    else if (cb == limitCombo) m_settings.frameLimit = kLimits[cb->SelectedIndex];
    else return;
    m_settings.Save();
    SafeApplySettings();
}

// Đưa giá trị hiện tại lên các công tắc (không kích hoạt lưu)
// Chiều cao popup Settings theo hướng màn hình: dọc -> cao tới 85% cửa sổ; ngang -> vừa chiều cao màn hình
// (trừ lề + padding của FlyoutPresenter). Phần dư cuộn trong settingsScroll.
void MainPage::FitSettingsHeight()
{
    auto b = Window::Current->Bounds;
    bool portrait = b.Height >= b.Width;
    double h = portrait ? b.Height * 0.85 : b.Height - 40.0;
    settingsScroll->MaxHeight = h > 120.0 ? h : 120.0;
}

// Gọi SyncSettingsUiImpl an toàn: luôn trả m_syncingUi về false (nếu kẹt true, mọi công tắc bị bỏ qua),
// lỗi thì hiện lên màn hình thay vì im lặng.
void MainPage::SyncSettingsUi()
{
    try { SyncSettingsUiImpl(); }
    catch (Platform::Exception^ ex)
    {
        wchar_t hr[32]; swprintf_s(hr, L" (0x%08X)", (unsigned)ex->HResult);
        ShowError("Settings UI error: " + ex->Message + ref new Platform::String(hr));
    }
    m_syncingUi = false;
}

void MainPage::SyncSettingsUiImpl()
{
    FitSettingsHeight();
    m_syncingUi = true;
    bool gpuOk = m_player && m_player->GpuAvailable();
    gpuSwitch->IsEnabled = gpuOk;
    gpuSwitch->IsOn = gpuOk && m_settings.gpu;
    bool msaaOk = m_player && m_player->MsaaSupported();
    msaaSwitch->IsEnabled = msaaOk && m_settings.gpu;
    msaaSwitch->IsOn = msaaOk && (m_settings.msaa < 0 ? m_player->MsaaDefault() : m_settings.msaa == 1);
    fpsSwitch->IsOn = m_settings.showFps;
    audioSwitch->IsOn = m_settings.audio;
    int si = 0, li = 0;
    for (int k = 0; k < 4; ++k) if (kScales[k] == m_settings.renderScale) si = k;
    for (int k = 0; k < 3; ++k) if (kLimits[k] == m_settings.frameLimit) li = k;
    scaleCombo->SelectedIndex = si;
    qualityCombo->SelectedIndex = (m_settings.quality >= 0 && m_settings.quality <= 3) ? m_settings.quality : 2;
    limitCombo->SelectedIndex = li;
    settingsNote->Text = !gpuOk ? "GPU unavailable, using the CPU renderer."
                       : !msaaOk ? "This GPU does not support MSAA 4x."
                       : "Changing the renderer reloads the running game.";
    m_syncingUi = false;
}

void MainPage::OnSettingToggled(Platform::Object^ sender, RoutedEventArgs^)
{
    if (m_syncingUi || !m_player) return;
    // Phải ép sender về ToggleSwitch^ trước khi so: Object^ và ToggleSwitch^ là hai interface COM khác nhau,
    // so trực tiếp luôn sai -> cài đặt không đổi, công tắc bật về vị trí cũ.
    auto ts = dynamic_cast<ToggleSwitch^>(sender);
    if (ts == gpuSwitch) m_settings.gpu = gpuSwitch->IsOn;
    else if (ts == msaaSwitch) m_settings.msaa = msaaSwitch->IsOn ? 1 : 0;
    else if (ts == fpsSwitch) m_settings.showFps = fpsSwitch->IsOn;
    else if (ts == audioSwitch) m_settings.audio = audioSwitch->IsOn;
    else return;
    m_settings.Save();
    SafeApplySettings();
    SyncSettingsUi();
}

void MainPage::UpdateFpsText()
{
    if (!m_player) return;
    wchar_t buf[96];
    swprintf_s(buf, L"%.0f FPS  %.1f ms  %s%s", m_player->Fps(), m_player->FrameWorkMs(),
               m_player->UsingGpu() ? L"GPU" : L"CPU", m_player->UsingGpu() && m_player->MsaaOn() ? L" MSAA" : L"");
    fpsText->Text = ref new Platform::String(buf);
}

void MainPage::ShowError(Platform::String^ message)
{
    DebugLogW((std::wstring(L"ERROR: ") + message->Data() + L"\n").c_str());
    errorText->Text = message;
    errorText->Visibility = Windows::UI::Xaml::Visibility::Visible;
    welcomePanel->Visibility = Windows::UI::Xaml::Visibility::Visible;
}


// ---------------------------------------------------------------- zoom 2 ngón
// Zoom ở tầng hiển thị (ma trận swapchain), 1x..4x, tâm là điểm giữa 2 ngón; di chuyển 2 ngón để dịch.

void MainPage::BeginPinch()
{
    // Huỷ thao tác của ngón đầu: nhả chuột trái (bỏ qua lần nhấc tay sau), dừng đếm giữ 8 giây
    if (m_touchActive)
    {
        m_longPress->Stop();
        m_touchActive = false;
        if (!m_suppressTouchUp) m_player->MouseButton(2, 0, m_lastTouch.X, m_lastTouch.Y);
        m_suppressTouchUp = false;
    }
    m_pinching = true;
    auto it = m_touches.begin();
    auto a = it->second; ++it;
    auto b = it->second;
    m_pinchMid = Windows::Foundation::Point((a.X + b.X) / 2, (a.Y + b.Y) / 2);
    m_pinchDist = std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y));
}

void MainPage::UpdatePinch()
{
    if (m_touches.size() < 2 || m_pinchDist < 1.0f) return;
    auto it = m_touches.begin();
    auto a = it->second; ++it;
    auto b = it->second;
    Windows::Foundation::Point mid((a.X + b.X) / 2, (a.Y + b.Y) / 2);
    float dist = std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y));

    float zoom = m_player->Zoom();
    float newZoom = (std::max)(1.0f, (std::min)(4.0f, zoom * dist / m_pinchDist));
    float k = newZoom / zoom;
    // Giữ điểm nội dung dưới tâm cũ ở dưới tâm mới (zoom quanh tâm + dịch theo tâm)
    float tx = mid.X - (m_pinchMid.X - m_player->ViewX()) * k;
    float ty = mid.Y - (m_pinchMid.Y - m_player->ViewY()) * k;
    m_player->SetView(newZoom, tx, ty);

    m_pinchMid = mid;
    m_pinchDist = dist;
}

// Nút Settings ở màn hình chào: mở cùng popup với nút Settings trên thanh dưới
void MainPage::OnWelcomeSettingsClick(Platform::Object^, RoutedEventArgs^)
{
    SyncSettingsUi();
    settingsFlyout->ShowAt(welcomeSettingsButton);
}

// Nút Settings trên thanh dưới: đóng CommandBar trước, rồi mở popup từ điểm neo ngoài thanh
// (popup gắn vào AppBarButton bị đóng theo thanh khi mở dropdown ComboBox -> không đổi được giá trị)
void MainPage::OnBarSettingsClick(Platform::Object^, RoutedEventArgs^)
{
    bottomBar->IsOpen = false;
    settingsFlyout->ShowAt(settingsAnchor);
}

void MainPage::SafeApplySettings()
{
    try { ApplySettings(); }
    catch (Platform::Exception^ ex)
    {
        wchar_t hr[32]; swprintf_s(hr, L" (0x%08X)", (unsigned)ex->HResult);
        ShowError("Apply settings error: " + ex->Message + ref new Platform::String(hr));
    }
}
