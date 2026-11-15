#pragma once
#include "MainPage.g.h"

namespace RuffleWP81
{
    public ref class MainPage sealed
    {
    public:
        MainPage();

    private:
        void OnLoaded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Log(Platform::String^ line);
        void InitD3D();
        void TestRuffle();

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    };
}
