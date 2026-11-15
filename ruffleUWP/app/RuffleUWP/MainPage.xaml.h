#pragma once
#include "MainPage.g.h"
#include "RufflePlayer.h"
#include "Settings.h"

namespace RuffleUWP
{
    public ref class MainPage sealed
    {
    public:
        MainPage();
        void OpenFile(Windows::Storage::StorageFile^ file);
        void SetSuspended(bool suspended);
        // Hiện lỗi lên màn hình (thay vì crash) — để chẩn đoán trên máy không đọc được log
        void ShowError(Platform::String^ message);
    private:
        void OnOpenClick(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnKeyboardClick(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnSettingToggled(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnWelcomeSettingsClick(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void ApplySettings();
        void SyncSettingsUi();
        void SyncSettingsUiImpl();
        void SafeApplySettings();
        void FitSettingsHeight();
        void UpdateFpsText();
        void OnBarSettingsClick(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnSettingComboChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
        void HandlePointerPressed(Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
        void HandlePointerMoved(Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
        void HandlePointerReleased(Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
        void HandlePointerLost(Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
        void HandleButtonUpdate(Windows::UI::Input::PointerUpdateKind kind, Windows::Foundation::Point pos, bool isTouch);
        void OnLongPress(Platform::Object^ sender, Platform::Object^ e);
        void ShowContextMenu(Windows::Foundation::Point pos);
        void OnKey(Windows::UI::Core::KeyEventArgs^ e, bool down);
        void OnBack(Platform::Object^ sender, Windows::UI::Core::BackRequestedEventArgs^ e);
        static uint32_t Modifiers();

        std::unique_ptr<RufflePlayer> m_player;
        bool m_sinkFocused = false;
        ULONGLONG m_lastBack = 0;
        bool m_backHooked = false;

        // Chạm giữ 8 giây -> context menu (chuột phải)
        Windows::UI::Xaml::DispatcherTimer^ m_longPress;
        bool m_touchActive = false;        // đang có ngón tay giữ (đã gửi chuột trái down)
        bool m_suppressTouchUp = false;    // đã nhả trái lúc mở menu -> bỏ qua lần nhấc tay
        Windows::Foundation::Point m_touchStart;
        bool m_menuOpen = false;
        AppSettings m_settings;
        bool m_syncingUi = false;
        Windows::UI::Xaml::DispatcherTimer^ m_fpsTimer;

        // Zoom 2 ngón: các ngón đang chạm (PointerId -> vị trí DIP)
        std::map<unsigned, Windows::Foundation::Point> m_touches;
        bool m_pinching = false;            // đang zoom: bỏ qua mọi sự kiện chạm cho tới khi nhấc hết ngón
        Windows::Foundation::Point m_pinchMid;
        float m_pinchDist = 0;
        Windows::Foundation::Point m_lastTouch; // vị trí ngón đầu (để nhả chuột khi bắt đầu zoom)
        void BeginPinch();
        void UpdatePinch();
    };
}
