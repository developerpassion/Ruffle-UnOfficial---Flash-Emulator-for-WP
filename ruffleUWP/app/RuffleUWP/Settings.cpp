#include "pch.h"
#include "Settings.h"

using namespace RuffleUWP;
using namespace Windows::Data::Json;

static std::wstring SettingsPath()
{
    return std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) + L"\\settings.json";
}

AppSettings AppSettings::Load()
{
    AppSettings s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, SettingsPath().c_str(), L"rb") != 0 || !f) return s;
    std::string utf8;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) utf8.append(buf, n);
    fclose(f);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring w(wlen, L'\0');
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &w[0], wlen);

    JsonObject^ o = nullptr;
    if (!JsonObject::TryParse(ref new Platform::String(w.c_str()), &o) || !o) return s; // file hỏng -> mặc định
    try
    {
        if (o->HasKey("gpu")) s.gpu = o->GetNamedBoolean("gpu");
        if (o->HasKey("msaa")) s.msaa = o->GetNamedBoolean("msaa") ? 1 : 0;
        if (o->HasKey("showFps")) s.showFps = o->GetNamedBoolean("showFps");
        if (o->HasKey("renderScale")) s.renderScale = (float)o->GetNamedNumber("renderScale");
        if (o->HasKey("quality"))
        {
            std::wstring q = o->GetNamedString("quality")->Data();
            s.quality = q == L"low" ? 0 : q == L"medium" ? 1 : q == L"best" ? 3 : 2;
        }
        if (o->HasKey("audio")) s.audio = o->GetNamedBoolean("audio");
        if (o->HasKey("frameLimit")) s.frameLimit = (int)o->GetNamedNumber("frameLimit");
    }
    catch (Platform::Exception^) {} // sai kiểu giá trị -> giữ phần đã đọc được
    return s;
}

void AppSettings::Save() const
{
    auto o = ref new JsonObject();
    o->SetNamedValue("gpu", JsonValue::CreateBooleanValue(gpu));
    if (msaa >= 0) o->SetNamedValue("msaa", JsonValue::CreateBooleanValue(msaa == 1));
    o->SetNamedValue("showFps", JsonValue::CreateBooleanValue(showFps));
    o->SetNamedValue("renderScale", JsonValue::CreateNumberValue(renderScale));
    static const wchar_t* qn[] = { L"low", L"medium", L"high", L"best" };
    o->SetNamedValue("quality", JsonValue::CreateStringValue(ref new Platform::String(qn[quality < 0 || quality > 3 ? 2 : quality])));
    o->SetNamedValue("audio", JsonValue::CreateBooleanValue(audio));
    o->SetNamedValue("frameLimit", JsonValue::CreateNumberValue(frameLimit));
    std::wstring w = o->Stringify()->Data();

    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &utf8[0], len, nullptr, nullptr);

    FILE* f = nullptr;
    if (_wfopen_s(&f, SettingsPath().c_str(), L"wb") != 0 || !f) return;
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
}
