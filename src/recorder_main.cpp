#include <Windows.h>
#include <CommCtrl.h>
#include <ShObjIdl.h>
#include <ShlObj_core.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "neon/MediaSession.hpp"
#include "neon/Recorder.hpp"

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t windowClassName[] = L"NeonRecorderWindow";
constexpr int applicationIcon = 101;
constexpr UINT_PTR uiTimer = 1;
constexpr UINT mediaChangedMessage = WM_APP + 1;
constexpr UINT autoSaveCompletedMessage = WM_APP + 2;
constexpr std::int64_t mediaTicksPerSecond = 10'000'000;
constexpr std::uint64_t automaticBoundaryGraceMilliseconds = 2'000;
constexpr wchar_t recorderSettingsKey[] = L"Software\\NeonJukebox\\Recorder";
constexpr wchar_t settingAutomaticEnabled[] = L"AutomaticEnabled";
constexpr wchar_t settingAutomaticSource[] = L"AutomaticSource";
constexpr wchar_t settingAutomaticFormat[] = L"AutomaticFormat";
constexpr wchar_t settingAutomaticFolder[] = L"AutomaticFolder";
constexpr wchar_t settingAutomaticSkipExisting[] = L"AutomaticSkipExisting";

enum ControlId : int {
    idHeader = 100,
    idSubtitle,
    idFormat,
    idStatus,
    idClock,
    idRecord,
    idStop,
    idLeftLevel,
    idRightLevel,
    idAutoMode,
    idAutoSkip,
    idAutoSource,
    idAutoFormat,
    idAutoFolder,
    idAutoPath,
    idLastAutoSave,
    idMetadataHeader,
    idTitleLabel,
    idTitle,
    idArtistLabel,
    idArtist,
    idAlbumLabel,
    idAlbum,
    idYearLabel,
    idYear,
    idArtworkStatus,
    idSaveWav,
    idSaveMp3,
    idNewRecording,
    idAutoNotice,
    idHint
};

std::wstring controlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring value(static_cast<std::size_t>(length + 1), L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

std::optional<DWORD> readDwordSetting(const wchar_t* name) {
    DWORD value{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, recorderSettingsKey, name, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS) return std::nullopt;
    return value;
}

std::wstring readStringSetting(const wchar_t* name) {
    DWORD size{};
    if (RegGetValueW(HKEY_CURRENT_USER, recorderSettingsKey, name, RRF_RT_REG_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, recorderSettingsKey, name, RRF_RT_REG_SZ,
                     nullptr, value.data(), &size) != ERROR_SUCCESS) return {};
    value.resize((size / sizeof(wchar_t)) - 1);
    return value;
}

void writeDwordSetting(const wchar_t* name, DWORD value) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, recorderSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

void writeStringSetting(const wchar_t* name, const std::wstring& value) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, recorderSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()), size);
    RegCloseKey(key);
}

void setFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

std::wstring timeText(std::uint64_t sampleFrames) {
    const std::uint64_t seconds = sampleFrames / neon::recorderSampleRate;
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02llu:%02llu:%02llu",
               static_cast<unsigned long long>(seconds / 3600),
               static_cast<unsigned long long>((seconds / 60) % 60),
               static_cast<unsigned long long>(seconds % 60));
    return buffer;
}

std::filesystem::path defaultRecordingFolder() {
    PWSTR musicPath{};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, KF_FLAG_DEFAULT, nullptr, &musicPath)) &&
        musicPath) {
        std::filesystem::path result = std::filesystem::path(musicPath) / L"Neon Recorder";
        CoTaskMemFree(musicPath);
        return result;
    }
    if (musicPath) CoTaskMemFree(musicPath);
    return std::filesystem::current_path() / L"Neon Recorder";
}

std::wstring timestampName() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t value[48]{};
    swprintf_s(value, L"Kayıt %04u-%02u-%02u %02u-%02u-%02u",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return value;
}

const wchar_t* mediaSourceName(neon::MediaSource source) {
    switch (source) {
        case neon::MediaSource::Spotify: return L"Spotify";
        case neon::MediaSource::Chrome: return L"Chrome";
        case neon::MediaSource::YouTube: return L"YouTube";
        case neon::MediaSource::Automatic: return L"aktif uygulama";
    }
    return L"medya uygulaması";
}

struct AutoSaveJob {
    neon::CapturedRecording recording;
    neon::RecordingMetadata metadata;
    std::filesystem::path folder;
    std::wstring trackKey;
    bool mp3{true};
    bool replaceExisting{};
    std::shared_ptr<void> rawFileLease;
};

struct AutoSaveResult {
    bool success{};
    bool skippedShortFragment{};
    std::filesystem::path destination;
    std::filesystem::path retainedRawPcm;
    std::wstring trackKey;
    std::wstring error;
};

std::wstring automaticTrackKey(const neon::RecordingMetadata& metadata) {
    std::wstring key = metadata.artist + L"\n" + metadata.album + L"\n" + metadata.title;
    std::ranges::transform(key, key.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return key;
}

class AutoSaveQueue {
public:
    ~AutoSaveQueue() { shutdown(); }

    void start(HWND notificationWindow) {
        window_ = notificationWindow;
        stopping_ = false;
        worker_ = std::thread([this] { run(); });
    }

    void enqueue(AutoSaveJob job) {
        job.rawFileLease = holdRawRecording(job.recording.rawPcmPath);
        {
            std::lock_guard lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        condition_.notify_one();
    }

    std::vector<AutoSaveResult> takeResults() {
        std::lock_guard lock(mutex_);
        std::vector<AutoSaveResult> values;
        while (!results_.empty()) {
            values.push_back(std::move(results_.front()));
            results_.pop_front();
        }
        return values;
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    static std::shared_ptr<void> holdRawRecording(const std::filesystem::path& path) {
        const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL |
                                              FILE_FLAG_SEQUENTIAL_SCAN,
                                          nullptr);
        if (handle == INVALID_HANDLE_VALUE) return {};
        return {handle, [](void* value) { CloseHandle(static_cast<HANDLE>(value)); }};
    }

    static bool waitForRawRecording(AutoSaveJob& job, std::wstring& error) {
        constexpr unsigned int attempts = 9;
        constexpr auto retryDelay = std::chrono::milliseconds(250);
        const std::uint64_t expectedBytes = job.recording.sampleFrames *
            neon::recorderChannels * neon::recorderBitsPerSample / 8;
        std::uint64_t availableBytes{};

        for (unsigned int attempt = 0; attempt < attempts; ++attempt) {
            std::error_code fileError;
            availableBytes = std::filesystem::file_size(job.recording.rawPcmPath, fileError);
            if (!fileError && availableBytes >= expectedBytes) {
                if (!job.rawFileLease) {
                    job.rawFileLease = holdRawRecording(job.recording.rawPcmPath);
                }
                if (job.rawFileLease) return true;
            }
            if (attempt + 1 < attempts) std::this_thread::sleep_for(retryDelay);
        }

        error = L"Geçici ham kayıt bulunamadı, açılamadı veya tamamlanmamış.\n\nYol:\n" +
                job.recording.rawPcmPath.wstring() + L"\n\nBeklenen boyut: " +
                std::to_wstring(expectedBytes) + L" bayt\nBulunan boyut: " +
                std::to_wstring(availableBytes) + L" bayt";
        return false;
    }

    static std::filesystem::path destinationFor(const AutoSaveJob& job,
                                                const std::filesystem::path& recordingFolder) {
        std::wstring base = job.metadata.artist.empty()
                                ? job.metadata.title
                                : job.metadata.artist + L" - " + job.metadata.title;
        if (base.empty()) base = timestampName();
        base = neon::safeRecordingFileName(std::move(base));
        const std::wstring extension = job.mp3 ? L".mp3" : L".wav";
        auto candidate = recordingFolder / (base + extension);
        if (job.replaceExisting) return candidate;
        std::error_code ignored;
        for (unsigned int copy = 2; std::filesystem::exists(candidate, ignored); ++copy) {
            candidate = recordingFolder /
                        (base + L" (" + std::to_wstring(copy) + L")" + extension);
            ignored.clear();
        }
        return candidate;
    }

    void run() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (true) {
            AutoSaveJob job;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) {
                    if (stopping_) break;
                    continue;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }

            AutoSaveResult result;
            result.trackKey = job.trackKey;
            try {
                if (waitForRawRecording(job, result.error)) {
                    const auto recordingFolder = neon::automaticRecordingFolder(
                        job.folder, job.metadata.artist, job.metadata.album);
                    std::error_code folderError;
                    std::filesystem::create_directories(recordingFolder, folderError);
                    if (folderError) {
                        result.error = L"Sanatçı/albüm kayıt klasörü oluşturulamadı: " +
                                       recordingFolder.wstring();
                    } else if (job.mp3 && FAILED(comResult)) {
                        result.error = L"Arka plan MP3 kodlayıcısı başlatılamadı.";
                    } else {
                        std::uint64_t audibleFrames{};
                        std::wstring analysisError;
                        if (neon::measureAudibleRecording(job.recording.rawPcmPath,
                                                           job.recording.sampleFrames,
                                                           audibleFrames, analysisError) &&
                            !neon::automaticRecordingIsLongEnough(audibleFrames)) {
                            result.skippedShortFragment = true;
                        } else {
                            result.destination = destinationFor(job, recordingFolder);
                            result.success = job.mp3
                                ? neon::writeHighQualityMp3(
                                      job.recording.rawPcmPath,
                                      job.recording.sampleFrames,
                                      result.destination, job.metadata, result.error)
                                : neon::writeCdQualityWave(
                                      job.recording.rawPcmPath,
                                      job.recording.sampleFrames,
                                      result.destination, job.metadata, result.error);
                            if (result.success && job.replaceExisting) {
                                neon::removeSupersededAutomaticRecordings(
                                    job.folder, job.metadata, result.destination);
                            }
                        }
                    }
                }
            } catch (...) {
                result.error = L"Otomatik kayıt sırasında beklenmeyen bir dosya hatası oluştu.";
            }
            job.rawFileLease.reset();
            if (result.success || result.skippedShortFragment) {
                std::error_code ignored;
                std::filesystem::remove(job.recording.rawPcmPath, ignored);
            } else {
                result.retainedRawPcm = job.recording.rawPcmPath;
            }
            {
                std::lock_guard lock(mutex_);
                results_.push_back(std::move(result));
            }
            if (window_) PostMessageW(window_, autoSaveCompletedMessage, 0, 0);
        }
        if (SUCCEEDED(comResult)) CoUninitialize();
    }

    HWND window_{};
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<AutoSaveJob> jobs_;
    std::deque<AutoSaveResult> results_;
    std::thread worker_;
    bool stopping_{};
};

class RecorderWindow {
public:
    explicit RecorderWindow(HWND window) : window_(window) {}

    bool create() {
        backgroundBrush_ = CreateSolidBrush(RGB(12, 15, 22));
        editBrush_ = CreateSolidBrush(RGB(25, 31, 42));
        headerFont_ = CreateFontW(-31, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        sectionFont_ = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        normalFont_ = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        buttonFont_ = CreateFontW(-19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        clockFont_ = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        if (!backgroundBrush_ || !editBrush_ || !headerFont_ || !sectionFont_ ||
            !normalFont_ || !buttonFont_ || !clockFont_) return false;

        header_ = label(idHeader, L"NEON RECORDER", 34, 22, 330, 42);
        subtitle_ = label(idSubtitle, L"Jukebox ve sistem çıkışındaki sesi kaydedin", 37, 64, 430, 25);
        format_ = label(idFormat, L"CD WAV  •  MP3 320 kbps  •  44.1 kHz  •  Stereo",
                        366, 30, 350, 28, SS_CENTER);
        status_ = label(idStatus, L"Kayıt için hazır", 38, 111, 440, 27);
        clock_ = label(idClock, L"00:00:00", 535, 103, 180, 40, SS_RIGHT);

        record_ = button(idRecord, L"●  KAYDI BAŞLAT", 38, 154, 326, 62);
        stop_ = button(idStop, L"■  DURDUR", 390, 154, 326, 62);
        leftLevel_ = progress(idLeftLevel, 38, 230, 326, 11);
        rightLevel_ = progress(idRightLevel, 390, 230, 326, 11);
        label(0, L"L", 20, 222, 18, 22, SS_CENTER);
        label(0, L"R", 716, 222, 18, 22, SS_CENTER);

        autoMode_ = CreateWindowExW(0, L"BUTTON", L"OTOMATİK KAYIT",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    38, 271, 195, 30, window_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(idAutoMode)),
                                    GetModuleHandleW(nullptr), nullptr);
        const bool savedAutomaticEnabled = readDwordSetting(settingAutomaticEnabled).value_or(1) != 0;
        SendMessageW(autoMode_, BM_SETCHECK,
                     savedAutomaticEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
        autoSkip_ = CreateWindowExW(0, L"BUTTON", L"OTOMATİK GEÇ",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                    38, 304, 195, 24, window_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(idAutoSkip)),
                                    GetModuleHandleW(nullptr), nullptr);
        const bool savedSkipExisting =
            readDwordSetting(settingAutomaticSkipExisting).value_or(0) != 0;
        SendMessageW(autoSkip_, BM_SETCHECK,
                     savedSkipExisting ? BST_CHECKED : BST_UNCHECKED, 0);
        autoSource_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                          CBS_DROPDOWNLIST | WS_VSCROLL,
                                      245, 269, 154, 150, window_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(idAutoSource)),
                                      GetModuleHandleW(nullptr), nullptr);
        const DWORD savedSource = readDwordSetting(settingAutomaticSource).value_or(0);
        const struct { neon::MediaSource source; const wchar_t* label; } sources[] = {
            {neon::MediaSource::Spotify, L"SPOTIFY"},
            {neon::MediaSource::Chrome, L"CHROME"},
            {neon::MediaSource::YouTube, L"YOUTUBE"},
            {neon::MediaSource::Automatic, L"AKTİF UYGULAMA"}
        };
        LRESULT selectedSourceIndex = 0;
        for (const auto& entry : sources) {
            const auto index = SendMessageW(autoSource_, CB_ADDSTRING, 0,
                                           reinterpret_cast<LPARAM>(entry.label));
            SendMessageW(autoSource_, CB_SETITEMDATA, index, static_cast<LPARAM>(entry.source));
            if (savedSource == static_cast<DWORD>(entry.source)) selectedSourceIndex = index;
        }
        SendMessageW(autoSource_, CB_SETCURSEL, selectedSourceIndex, 0);
        autoFormat_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                          CBS_DROPDOWNLIST | WS_VSCROLL,
                                      411, 269, 147, 120, window_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(idAutoFormat)),
                                      GetModuleHandleW(nullptr), nullptr);
        SendMessageW(autoFormat_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MP3 • 320 kbps"));
        SendMessageW(autoFormat_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WAV • CD kalite"));
        DWORD savedFormat = readDwordSetting(settingAutomaticFormat).value_or(0);
        if (savedFormat > 1) savedFormat = 0;
        SendMessageW(autoFormat_, CB_SETCURSEL, savedFormat, 0);
        autoFolderButton_ = button(idAutoFolder, L"KLASÖR SEÇ", 570, 268, 146, 34);
        autoFolder_ = readStringSetting(settingAutomaticFolder);
        if (autoFolder_.empty()) autoFolder_ = defaultRecordingFolder();
        autoPath_ = label(idAutoPath, autoFolder_.c_str(), 245, 307, 471, 24);
        lastAutoSave_ = label(idLastAutoSave, L"Son otomatik kayıt: Henüz yok", 38, 331, 678, 24);

        metadataHeader_ = label(idMetadataHeader, L"KAYIT BİLGİLERİ", 38, 381, 250, 28);
        artworkStatus_ = label(idArtworkStatus, L"Kapak: bekleniyor", 300, 383, 416, 24, SS_RIGHT);
        titleLabel_ = label(idTitleLabel, L"Parça adı *", 38, 422, 170, 25);
        title_ = edit(idTitle, 210, 416, 506, 35);
        artistLabel_ = label(idArtistLabel, L"Sanatçı", 38, 472, 170, 25);
        artist_ = edit(idArtist, 210, 466, 506, 35);
        albumLabel_ = label(idAlbumLabel, L"Albüm", 38, 522, 170, 25);
        album_ = edit(idAlbum, 210, 516, 356, 35);
        yearLabel_ = label(idYearLabel, L"Yıl", 580, 522, 36, 25);
        year_ = edit(idYear, 620, 516, 96, 35, ES_AUTOHSCROLL | ES_NUMBER);

        saveWav_ = button(idSaveWav, L"MANUEL WAV KAYDET", 38, 583, 210, 55);
        saveMp3_ = button(idSaveMp3, L"MANUEL MP3 KAYDET", 260, 583, 252, 55);
        newRecording_ = button(idNewRecording, L"YENİ KAYIT", 530, 583, 186, 55);
        autoNotice_ = label(idAutoNotice,
                            L"OTOMATİK: Parça değişince önceki kayıt kendiliğinden kaydedilir.",
                            38, 590, 678, 30, SS_CENTER | SS_CENTERIMAGE);
        hint_ = label(idHint,
                      L"Sistem çıkışı kaydedilir; baştaki ve sondaki sessizlik otomatik kırpılır.",
                      38, 659, 678, 30, SS_CENTER);

        for (HWND control : {subtitle_, format_, status_, autoMode_, autoSkip_, autoSource_, autoFormat_, autoPath_,
                             lastAutoSave_,
                             titleLabel_, artistLabel_, albumLabel_, yearLabel_, artworkStatus_,
                             autoNotice_, hint_}) {
            setFont(control, normalFont_);
        }
        setFont(header_, headerFont_);
        setFont(metadataHeader_, sectionFont_);
        setFont(clock_, clockFont_);
        for (HWND control : {record_, stop_, autoFolderButton_, saveWav_, saveMp3_, newRecording_}) {
            setFont(control, buttonFont_);
        }
        for (HWND control : {title_, artist_, album_, year_}) {
            setFont(control, normalFont_);
            SendMessageW(control, EM_SETLIMITTEXT, 240, 0);
            SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
        }
        SendMessageW(year_, EM_SETLIMITTEXT, 4, 0);
        setArtworkStatus(false);
        SetWindowTheme(leftLevel_, L"", L"");
        SetWindowTheme(rightLevel_, L"", L"");
        for (HWND level : {leftLevel_, rightLevel_}) {
            SendMessageW(level, PBM_SETRANGE32, 0, 1000);
            SendMessageW(level, PBM_SETBARCOLOR, 0, RGB(36, 230, 202));
            SendMessageW(level, PBM_SETBKCOLOR, 0, RGB(32, 39, 51));
        }
        setIdleControls();
        if (automaticModeEnabled()) {
            const std::wstring initialStatus = std::wstring(L"Otomatik mod açık — ") +
                                               mediaSourceName(selectedMediaSource()) +
                                               L" içinde parça bekleniyor";
            SetWindowTextW(status_, initialStatus.c_str());
        } else {
            SetWindowTextW(status_, L"Manuel kayıt için hazır");
        }
        SetTimer(window_, uiTimer, 80, nullptr);
        autoSaveQueue_.start(window_);
        mediaWatcher_.setSource(selectedMediaSource());
        mediaWatcher_.start([window = window_] {
            PostMessageW(window, mediaChangedMessage, 0, 0);
        });
        return true;
    }

    void destroy() {
        KillTimer(window_, uiTimer);
        mediaWatcher_.stop();
        recorder_.discard();
        autoSaveQueue_.shutdown();
        if (headerFont_) DeleteObject(headerFont_);
        if (sectionFont_) DeleteObject(sectionFont_);
        if (normalFont_) DeleteObject(normalFont_);
        if (buttonFont_) DeleteObject(buttonFont_);
        if (clockFont_) DeleteObject(clockFont_);
        if (backgroundBrush_) DeleteObject(backgroundBrush_);
        if (editBrush_) DeleteObject(editBrush_);
    }

    LRESULT colorControl(UINT message, HDC dc, HWND control) const {
        SetBkMode(dc, TRANSPARENT);
        if (message == WM_CTLCOLOREDIT) {
            SetTextColor(dc, RGB(238, 244, 250));
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, RGB(25, 31, 42));
            return reinterpret_cast<LRESULT>(editBrush_);
        }
        const int id = GetDlgCtrlID(control);
        if (id == idHeader || id == idMetadataHeader || id == idFormat || id == idAutoNotice) {
            SetTextColor(dc, RGB(44, 231, 205));
        } else if (id == idStatus && expectedRecording_) {
            SetTextColor(dc, RGB(255, 78, 105));
        } else if (id == idHint || id == idSubtitle || id == idAutoPath ||
                   id == idLastAutoSave || id == idArtworkStatus) {
            SetTextColor(dc, RGB(143, 157, 177));
        } else {
            SetTextColor(dc, RGB(224, 231, 239));
        }
        return reinterpret_cast<LRESULT>(backgroundBrush_);
    }

    void paint() const {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        FillRect(dc, &client, backgroundBrush_);
        HPEN separator = CreatePen(PS_SOLID, 1, RGB(43, 54, 70));
        const auto previousPen = SelectObject(dc, separator);
        MoveToEx(dc, 38, 96, nullptr);
        LineTo(dc, 716, 96);
        MoveToEx(dc, 38, 258, nullptr);
        LineTo(dc, 716, 258);
        MoveToEx(dc, 38, 365, nullptr);
        LineTo(dc, 716, 365);
        SelectObject(dc, previousPen);
        DeleteObject(separator);
        EndPaint(window_, &paint);
    }

    void command(int id) {
        switch (id) {
            case idRecord: startRecording(); break;
            case idStop:
                if (automaticCapture_) finishAutomaticRecording();
                else stopRecording();
                break;
            case idAutoMode: toggleAutomaticMode(); break;
            case idAutoSkip: toggleAutomaticSkip(); break;
            case idAutoSource: changeAutomaticSource(); break;
            case idAutoFormat: saveAutomaticSettings(); break;
            case idAutoFolder: chooseAutomaticFolder(); break;
            case idSaveWav: saveRecording(false); break;
            case idSaveMp3: saveRecording(true); break;
            case idNewRecording: resetRecording(true); break;
            default: break;
        }
    }

    void mediaChanged() {
        handleMediaSession(mediaWatcher_.latest());
    }

    void autoSaveCompleted() {
        for (auto& result : autoSaveQueue_.takeResults()) {
            pendingRecordingKeys_.erase(result.trackKey);
            if (result.skippedShortFragment) {
                if (!expectedRecording_) {
                    SetWindowTextW(status_, L"2 dakika veya daha kısa kayıt kaydedilmedi");
                }
                continue;
            }
            if (result.success) {
                const std::wstring saved = L"Son otomatik kayıt: " + result.destination.wstring();
                SetWindowTextW(lastAutoSave_, saved.c_str());
                const std::wstring status = expectedRecording_ && automaticCapture_
                    ? L"● OTOMATİK KAYIT — önceki parça kaydedildi"
                    : L"Otomatik kaydedildi: " + result.destination.wstring();
                SetWindowTextW(status_, status.c_str());
            } else {
                std::wstring message = result.error;
                if (!result.retainedRawPcm.empty()) {
                    message += L"\n\nHam kayıt korunuyor:\n" + result.retainedRawPcm.wstring();
                }
                SetWindowTextW(lastAutoSave_, L"Son otomatik kayıt: KAYDETME HATASI");
                MessageBoxW(window_, message.c_str(), L"Otomatik kayıt kaydedilemedi",
                            MB_OK | MB_ICONERROR);
            }
        }
    }

    void timer() {
        const auto snapshot = recorder_.snapshot();
        SetWindowTextW(clock_, timeText(snapshot.sampleFrames).c_str());
        SendMessageW(leftLevel_, PBM_SETPOS,
                     static_cast<WPARAM>(std::clamp(snapshot.peakLeft, 0.0F, 1.0F) * 1000.0F), 0);
        SendMessageW(rightLevel_, PBM_SETPOS,
                     static_cast<WPARAM>(std::clamp(snapshot.peakRight, 0.0F, 1.0F) * 1000.0F), 0);
        if (expectedRecording_ && !snapshot.recording) {
            if (automaticCapture_) {
                finishAutomaticRecording();
                return;
            }
            std::wstring error;
            const bool hasAudio = recorder_.stop(error);
            expectedRecording_ = false;
            unsaved_ = hasAudio;
            setStoppedControls(hasAudio);
            if (!error.empty()) {
                SetWindowTextW(status_, hasAudio ? L"Kayıt kesildi; elde edilen bölüm kaydedilebilir"
                                                 : L"Kayıt durdu");
                MessageBoxW(window_, error.c_str(), L"Neon Recorder", MB_OK | MB_ICONWARNING);
            }
            InvalidateRect(window_, nullptr, FALSE);
        }
        if (automaticCapture_ && autoStopAt_ && GetTickCount64() >= autoStopAt_) {
            autoStopAt_ = 0;
            if (!lastMedia_.playing) finishAutomaticRecording();
        }
    }

    bool canClose() {
        if (automaticCapture_) {
            const int answer = MessageBoxW(window_,
                                           L"Otomatik kayıt devam ediyor. Kaydı tamamlayıp çıkmak istiyor musunuz?",
                                           L"Neon Recorder", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
            if (answer != IDYES) return false;
            finishAutomaticRecording();
        } else if (expectedRecording_) {
            const int answer = MessageBoxW(window_, L"Kayıt devam ediyor. Kaydı silip çıkmak istiyor musunuz?",
                                           L"Neon Recorder", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) return false;
        } else if (unsaved_) {
            const int answer = MessageBoxW(window_, L"Kaydedilmemiş kayıt silinecek. Çıkmak istiyor musunuz?",
                                           L"Neon Recorder", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) return false;
        }
        return true;
    }

private:
    HWND label(int id, const wchar_t* text, int x, int y, int width, int height,
               DWORD alignment = SS_LEFT) const {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | alignment,
                               x, y, width, height, window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    }

    HWND button(int id, const wchar_t* text, int x, int y, int width, int height) const {
        return CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               x, y, width, height, window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    }

    HWND edit(int id, int x, int y, int width, int height,
              DWORD editStyle = ES_AUTOHSCROLL) const {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | editStyle,
                               x, y, width, height, window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    }

    HWND progress(int id, int x, int y, int width, int height) const {
        return CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                               x, y, width, height, window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    }

    bool automaticModeEnabled() const {
        return SendMessageW(autoMode_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    bool automaticSkipEnabled() const {
        return SendMessageW(autoSkip_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    neon::MediaSource selectedMediaSource() const {
        const auto index = SendMessageW(autoSource_, CB_GETCURSEL, 0, 0);
        if (index == CB_ERR) return neon::MediaSource::Spotify;
        switch (SendMessageW(autoSource_, CB_GETITEMDATA, index, 0)) {
            case 1: return neon::MediaSource::Chrome;
            case 2: return neon::MediaSource::Automatic;
            case 3: return neon::MediaSource::YouTube;
            default: return neon::MediaSource::Spotify;
        }
    }

    void saveAutomaticSettings() const {
        writeDwordSetting(settingAutomaticEnabled, automaticModeEnabled() ? 1 : 0);
        writeDwordSetting(settingAutomaticSkipExisting, automaticSkipEnabled() ? 1 : 0);
        const auto format = SendMessageW(autoFormat_, CB_GETCURSEL, 0, 0);
        writeDwordSetting(settingAutomaticSource,
                          static_cast<DWORD>(selectedMediaSource()));
        writeDwordSetting(settingAutomaticFormat,
                          static_cast<DWORD>(format == CB_ERR ? 0 : format));
        writeStringSetting(settingAutomaticFolder, autoFolder_);
    }

    void changeAutomaticSource() {
        saveAutomaticSettings();
        updateModeVisibility();
        if (automaticCapture_) finishAutomaticRecording();
        lastMedia_ = {};
        autoSkippedTrackKey_.clear();
        const auto source = selectedMediaSource();
        mediaWatcher_.setSource(source);
        setMediaFields(lastMedia_, false);
        automaticArtwork_.clear();
        automaticArtworkMimeType_.clear();
        setArtworkStatus(false);
        const std::wstring status = std::wstring(L"Otomatik kaynak: ") +
                                    mediaSourceName(source) + L" — parça bekleniyor";
        SetWindowTextW(status_, status.c_str());
    }

    void chooseAutomaticFolder() {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog)))) return;
        dialog->SetTitle(L"Otomatik kayıt klasörünü seçin");
        DWORD options{};
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                               FOS_PATHMUSTEXIST);
        }
        ComPtr<IShellItem> currentFolder;
        if (SUCCEEDED(SHCreateItemFromParsingName(autoFolder_.c_str(), nullptr,
                                                  IID_PPV_ARGS(&currentFolder)))) {
            dialog->SetFolder(currentFolder.Get());
        }
        const HRESULT shown = dialog->Show(window_);
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(shown)) return;
        ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item))) return;
        PWSTR path{};
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
        autoFolder_ = path;
        CoTaskMemFree(path);
        SetWindowTextW(autoPath_, autoFolder_.c_str());
        saveAutomaticSettings();
        autoSkippedTrackKey_.clear();
        if (automaticModeEnabled() && lastMedia_.playing) {
            skipExistingRecording(lastMedia_, automaticCapture_);
        }
    }

    void setMediaFields(const neon::MediaSessionInfo& media, bool onlyWhenEmpty) const {
        auto set = [onlyWhenEmpty](HWND control, const std::wstring& value) {
            if (onlyWhenEmpty) {
                if (!value.empty() && GetWindowTextLengthW(control) == 0) {
                    SetWindowTextW(control, value.c_str());
                }
                return;
            }
            SetWindowTextW(control, value.c_str());
        };
        set(title_, media.title);
        set(artist_, media.artist);
        set(album_, media.album);
        if (!onlyWhenEmpty) SetWindowTextW(year_, L"");
    }

    void setArtworkStatus(bool available) const {
        SetWindowTextW(artworkStatus_, available
            ? L"Kapak: kaynaktan alındı • dosyaya gömülecek"
            : L"Kapak: kaynakta yok");
    }

    void rememberAutomaticArtwork(const neon::MediaSessionInfo& media) {
        if (!media.artwork.empty()) {
            automaticArtwork_ = media.artwork;
            automaticArtworkMimeType_ = media.artworkMimeType;
        }
        setArtworkStatus(!automaticArtwork_.empty());
    }

    neon::RecordingMetadata metadataFromMedia(const neon::MediaSessionInfo& media) const {
        return {media.title, media.artist, media.album};
    }

    void discardAutomaticCapture() {
        if (!automaticCapture_) return;
        recorder_.discard();
        expectedRecording_ = false;
        automaticCapture_ = false;
        unsaved_ = false;
        autoStopAt_ = 0;
        autoTrackKey_.clear();
        autoStartedAt_ = 0;
        autoTimelineAvailable_ = false;
        autoPositionTicks_ = 0;
        autoEndTicks_ = 0;
        autoDurationTicks_ = 0;
        automaticReplaceExisting_ = false;
        automaticArtwork_.clear();
        automaticArtworkMimeType_.clear();
        setIdleControls();
    }

    bool skipExistingRecording(const neon::MediaSessionInfo& media,
                               bool discardCurrentCapture) {
        if (!automaticSkipEnabled()) {
            automaticReplaceExisting_ = false;
            return false;
        }
        const auto metadata = metadataFromMedia(media);
        const auto hasText = [](std::wstring_view value) {
            return value.find_first_not_of(L" \t\r\n") != std::wstring_view::npos;
        };
        const bool completeIdentity = hasText(metadata.title) && hasText(metadata.artist) &&
                                      hasText(metadata.album);
        const std::wstring trackKey = automaticTrackKey(metadata);
        const auto durationTicks = media.durationTicks > 0
            ? static_cast<std::uint64_t>(media.durationTicks) : 0;
        const auto pending = pendingRecordingKeys_.find(trackKey);
        const bool pendingExists = pending != pendingRecordingKeys_.end();
        const bool pendingDurationMatch = pendingExists &&
            neon::automaticRecordingDurationMatches(pending->second, durationTicks);
        const bool fileExists = completeIdentity &&
            neon::automaticRecordingAlreadyExists(autoFolder_, metadata);
        const bool fileDurationMatch = completeIdentity && durationTicks > 0 &&
            neon::automaticRecordingMatchesDuration(autoFolder_, metadata, durationTicks);
        const bool alreadyRecorded = completeIdentity &&
                                     (pendingDurationMatch || fileDurationMatch);
        if (!alreadyRecorded) {
            automaticReplaceExisting_ = completeIdentity && durationTicks > 0 &&
                                        (pendingExists || fileExists);
            if (trackKey != autoSkippedTrackKey_) autoSkippedTrackKey_.clear();
            return false;
        }

        automaticReplaceExisting_ = false;
        if (discardCurrentCapture) discardAutomaticCapture();
        setMediaFields(media, false);
        setArtworkStatus(!media.artwork.empty());
        setMetadataFieldsState(true, true);
        if (media.nextEnabled) {
            if (trackKey != autoSkippedTrackKey_) {
                autoSkippedTrackKey_ = trackKey;
                mediaWatcher_.requestSkipNext();
            }
            SetWindowTextW(status_, L"Zaten kayıtlı — sonraki parçaya geçiliyor");
        } else {
            SetWindowTextW(status_, L"Zaten kayıtlı — kaynak sonraki parça komutunu desteklemiyor");
        }
        InvalidateRect(window_, nullptr, FALSE);
        return true;
    }

    void toggleAutomaticSkip() {
        saveAutomaticSettings();
        autoSkippedTrackKey_.clear();
        if (!automaticModeEnabled()) return;
        if (lastMedia_.playing &&
            skipExistingRecording(lastMedia_, automaticCapture_)) return;
        if (!automaticCapture_ && !expectedRecording_ && !unsaved_) {
            handleMediaSession(lastMedia_);
        }
    }

    void beginAutomaticRecording(const neon::MediaSessionInfo& media) {
        if (expectedRecording_ || unsaved_) return;
        recorder_.discard();
        SetWindowTextW(title_, L"");
        SetWindowTextW(artist_, L"");
        SetWindowTextW(album_, L"");
        SetWindowTextW(year_, L"");
        automaticArtwork_.clear();
        automaticArtworkMimeType_.clear();
        setMediaFields(media, false);
        rememberAutomaticArtwork(media);
        SetWindowTextW(clock_, L"00:00:00");

        std::wstring error;
        if (!recorder_.start(autoFolder_, error)) {
            setIdleControls();
            SetWindowTextW(status_, L"Otomatik kayıt başlatılamadı");
            MessageBoxW(window_, error.c_str(), L"Neon Recorder", MB_OK | MB_ICONERROR);
            return;
        }
        expectedRecording_ = true;
        automaticCapture_ = true;
        autoTrackKey_ = neon::mediaTrackIdentity(media);
        autoStartedAt_ = GetTickCount64();
        rememberAutomaticTimeline(media);
        EnableWindow(record_, FALSE);
        EnableWindow(stop_, TRUE);
        EnableWindow(newRecording_, FALSE);
        setMetadataFieldsState(true, true);
        enableSaveButtons(false);
        const std::wstring status = std::wstring(L"● OTOMATİK KAYIT — ") +
                                    mediaSourceName(media.source) + L" parçası kaydediliyor";
        SetWindowTextW(status_, status.c_str());
        InvalidateRect(window_, nullptr, FALSE);
    }

    void finishAutomaticRecording() {
        if (!automaticCapture_) return;
        const std::uint64_t expectedDurationTicks = autoDurationTicks_;
        const bool replaceExisting = automaticReplaceExisting_;
        std::wstring captureError;
        const bool hasAudio = recorder_.stop(captureError);
        expectedRecording_ = false;
        automaticCapture_ = false;
        autoStopAt_ = 0;
        autoTrackKey_.clear();
        autoStartedAt_ = 0;
        autoTimelineAvailable_ = false;
        autoPositionTicks_ = 0;
        autoEndTicks_ = 0;
        autoDurationTicks_ = 0;
        automaticReplaceExisting_ = false;

        if (hasAudio) {
            neon::RecordingMetadata metadata{controlText(title_), controlText(artist_),
                                              controlText(album_), controlText(year_),
                                              automaticArtwork_, automaticArtworkMimeType_};
            if (metadata.title.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
                metadata.title = timestampName();
                SetWindowTextW(title_, metadata.title.c_str());
            }
            const bool mp3 = SendMessageW(autoFormat_, CB_GETCURSEL, 0, 0) != 1;
            auto recording = recorder_.takeRecording();
            if (recording) {
                if (!neon::automaticRecordingIsLongEnough(recording.sampleFrames)) {
                    std::error_code ignored;
                    std::filesystem::remove(recording.rawPcmPath, ignored);
                    SetWindowTextW(status_, L"2 dakika veya daha kısa kayıt kaydedilmedi");
                } else {
                    const std::wstring trackKey = automaticTrackKey(metadata);
                    const std::uint64_t capturedDurationTicks =
                        recording.sampleFrames * static_cast<std::uint64_t>(mediaTicksPerSecond) /
                        neon::recorderSampleRate;
                    pendingRecordingKeys_[trackKey] = expectedDurationTicks > 0
                        ? expectedDurationTicks : capturedDurationTicks;
                    autoSaveQueue_.enqueue({std::move(recording), std::move(metadata),
                                            autoFolder_, trackKey, mp3, replaceExisting});
                    SetWindowTextW(status_, mp3 ? L"Önceki parça MP3 olarak kaydediliyor…"
                                                : L"Önceki parça WAV olarak kaydediliyor…");
                }
            }
        } else {
            recorder_.discard();
            SetWindowTextW(status_, captureError.empty() ? L"Otomatik kayıtta ses bulunamadı"
                                                         : captureError.c_str());
        }
        automaticArtwork_.clear();
        automaticArtworkMimeType_.clear();
        setArtworkStatus(false);
        unsaved_ = false;
        setIdleControls();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void rememberAutomaticTimeline(const neon::MediaSessionInfo& media) {
        autoTimelineAvailable_ = media.timelineAvailable;
        autoPositionTicks_ = media.positionTicks;
        autoEndTicks_ = media.endTicks;
        if (media.durationTicks > 0) {
            autoDurationTicks_ = static_cast<std::uint64_t>(media.durationTicks);
        }
    }

    bool timelineIndicatesNewTrack(const neon::MediaSessionInfo& media) const {
        if (!autoTimelineAvailable_ || !media.timelineAvailable) return false;
        const auto capturedFrames = recorder_.snapshot().sampleFrames;
        if (capturedFrames < static_cast<std::uint64_t>(neon::recorderSampleRate) * 5) return false;
        if (media.positionTicks < 0 || media.positionTicks > 5 * mediaTicksPerSecond) return false;

        const bool jumpedBack = autoPositionTicks_ > media.positionTicks + 3 * mediaTicksPerSecond;
        const auto durationDelta = autoEndTicks_ > media.endTicks
            ? autoEndTicks_ - media.endTicks
            : media.endTicks - autoEndTicks_;
        const bool durationChanged = autoEndTicks_ > 0 && media.endTicks > 0 &&
                                     durationDelta > 2 * mediaTicksPerSecond;
        return jumpedBack || durationChanged;
    }

    void handleMediaSession(const neon::MediaSessionInfo& media) {
        if (media.requestedSource != selectedMediaSource()) return;
        lastMedia_ = media;
        if (!automaticModeEnabled()) return;
        if (!media.error.empty()) {
            if (!expectedRecording_) SetWindowTextW(status_, media.error.c_str());
            return;
        }
        if (!media.apiAvailable) return;
        if (!media.playing) {
            if (automaticCapture_ && !autoStopAt_) autoStopAt_ = GetTickCount64() + 500;
            if (!expectedRecording_ && !unsaved_) {
                setMediaFields(media, false);
                setArtworkStatus(!media.artwork.empty());
                setMetadataFieldsState(true, true);
                const auto displayedSource = media.sessionAvailable
                    ? media.source
                    : media.requestedSource;
                const std::wstring status = std::wstring(mediaSourceName(displayedSource)) +
                    (media.sessionAvailable
                         ? L" hazır — parçanın başlaması bekleniyor"
                         : L" medya oturumu bekleniyor");
                SetWindowTextW(status_, status.c_str());
            }
            return;
        }

        autoStopAt_ = 0;
        const std::wstring newKey = neon::mediaTrackIdentity(media);
        if (automaticCapture_) {
            const bool metadataChanged = !autoTrackKey_.empty() && !newKey.empty() &&
                                         newKey != autoTrackKey_;
            const bool timelineChanged = timelineIndicatesNewTrack(media);
            const bool insideBoundaryGrace = autoStartedAt_ &&
                GetTickCount64() - autoStartedAt_ < automaticBoundaryGraceMilliseconds;
            if (metadataChanged || (timelineChanged && !insideBoundaryGrace)) {
                finishAutomaticRecording();
                if (!skipExistingRecording(media, false)) beginAutomaticRecording(media);
            } else if (autoTrackKey_.empty() && !newKey.empty()) {
                autoTrackKey_ = newKey;
                setMediaFields(media, false);
                rememberAutomaticArtwork(media);
            } else {
                // Absorb Spotify's delayed timeline/artist update into the
                // track that was just opened instead of splitting it again.
                setMediaFields(media, false);
                rememberAutomaticArtwork(media);
            }
            rememberAutomaticTimeline(media);
            setMetadataFieldsState(true, true);
            if (skipExistingRecording(media, true)) return;
            return;
        }
        if (!expectedRecording_ && !unsaved_ &&
            !skipExistingRecording(media, false)) {
            beginAutomaticRecording(media);
        }
    }

    void toggleAutomaticMode() {
        if (!automaticModeEnabled()) {
            saveAutomaticSettings();
            autoStopAt_ = 0;
            autoSkippedTrackKey_.clear();
            if (automaticCapture_) finishAutomaticRecording();
            setIdleControls();
            if (!expectedRecording_ && !unsaved_) SetWindowTextW(status_, L"Otomatik mod kapalı");
            return;
        }
        if ((expectedRecording_ && !automaticCapture_) || unsaved_) {
            SendMessageW(autoMode_, BM_SETCHECK, BST_UNCHECKED, 0);
            saveAutomaticSettings();
            setMetadataFieldsState(true, false);
            MessageBoxW(window_,
                        L"Otomatik modu açmadan önce manuel kaydı tamamlayın veya silin.",
                        L"Neon Recorder", MB_OK | MB_ICONINFORMATION);
            return;
        }
        saveAutomaticSettings();
        setIdleControls();
        if (!expectedRecording_ && !unsaved_) {
            const std::wstring status = std::wstring(L"Otomatik mod açık — ") +
                                        mediaSourceName(selectedMediaSource()) +
                                        L" içinde parça bekleniyor";
            SetWindowTextW(status_, status.c_str());
        }
        handleMediaSession(mediaWatcher_.latest());
    }

    void setMetadataFieldsState(bool enabled, bool readOnly) const {
        for (HWND control : {title_, artist_, album_, year_}) {
            SendMessageW(control, EM_SETREADONLY, readOnly ? TRUE : FALSE, 0);
            EnableWindow(control, enabled);
        }
    }

    void enableSaveButtons(bool enabled) const {
        for (HWND control : {saveWav_, saveMp3_}) EnableWindow(control, enabled);
    }

    void updateModeVisibility() const {
        const bool automatic = automaticModeEnabled();
        SetWindowTextW(hint_, automatic && selectedMediaSource() == neon::MediaSource::YouTube
            ? L"YouTube’u Chrome’da oynatın. Sistem çıkışı kaydedilir; diğer sesleri kapatın."
            : L"Sistem çıkışı kaydedilir; baştaki ve sondaki sessizlik otomatik kırpılır.");
        for (HWND control : {autoSkip_, autoSource_, autoFormat_, autoFolderButton_}) {
            EnableWindow(control, automatic);
        }
        for (HWND control : {saveWav_, saveMp3_, newRecording_}) {
            ShowWindow(control, automatic ? SW_HIDE : SW_SHOW);
        }
        ShowWindow(autoNotice_, automatic ? SW_SHOW : SW_HIDE);
    }

    void setIdleControls() const {
        updateModeVisibility();
        EnableWindow(record_, automaticModeEnabled() ? FALSE : TRUE);
        EnableWindow(stop_, FALSE);
        EnableWindow(newRecording_, FALSE);
        setMetadataFieldsState(automaticModeEnabled(), automaticModeEnabled());
        enableSaveButtons(false);
    }

    void setStoppedControls(bool hasAudio) const {
        updateModeVisibility();
        EnableWindow(record_, TRUE);
        EnableWindow(stop_, FALSE);
        EnableWindow(newRecording_, hasAudio);
        setMetadataFieldsState(hasAudio, false);
        enableSaveButtons(hasAudio);
        if (hasAudio) {
            SetWindowTextW(status_, L"Kayıt durdu — bilgileri doldurup kaydedin");
            SetFocus(title_);
        } else {
            SetWindowTextW(status_, L"Kaydedilmiş ses bulunamadı");
        }
    }

    void startRecording() {
        if (unsaved_) {
            const int answer = MessageBoxW(window_,
                                           L"Kaydedilmemiş kayıt silinsin ve yeni kayıt başlatılsın mı?",
                                           L"Yeni kayıt", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) return;
        }
        recorder_.discard();
        unsaved_ = false;
        automaticCapture_ = false;
        autoStopAt_ = 0;
        SetWindowTextW(title_, L"");
        SetWindowTextW(artist_, L"");
        SetWindowTextW(album_, L"");
        SetWindowTextW(year_, L"");
        automaticArtwork_.clear();
        automaticArtworkMimeType_.clear();
        setArtworkStatus(false);
        SetWindowTextW(clock_, L"00:00:00");
        std::wstring error;
        if (!recorder_.start(autoFolder_, error)) {
            setIdleControls();
            SetWindowTextW(status_, L"Kayıt başlatılamadı");
            MessageBoxW(window_, error.c_str(), L"Neon Recorder", MB_OK | MB_ICONERROR);
            return;
        }
        expectedRecording_ = true;
        EnableWindow(record_, FALSE);
        EnableWindow(stop_, TRUE);
        EnableWindow(newRecording_, FALSE);
        setMetadataFieldsState(true, false);
        enableSaveButtons(false);
        SetWindowTextW(status_, L"● KAYIT YAPILIYOR — parça bilgilerini şimdi girebilirsiniz");
        SetFocus(title_);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void stopRecording() {
        if (!expectedRecording_) return;
        std::wstring error;
        const bool hasAudio = recorder_.stop(error);
        expectedRecording_ = false;
        unsaved_ = hasAudio;
        setStoppedControls(hasAudio);
        if (!error.empty()) {
            MessageBoxW(window_, error.c_str(), L"Neon Recorder",
                        MB_OK | (hasAudio ? MB_ICONWARNING : MB_ICONERROR));
        }
        InvalidateRect(window_, nullptr, FALSE);
    }

    std::filesystem::path chooseDestination(const neon::RecordingMetadata& metadata,
                                            bool mp3) const {
        ComPtr<IFileSaveDialog> dialog;
        HRESULT result = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&dialog));
        if (FAILED(result)) return {};
        dialog->SetTitle(mp3 ? L"320 kbps MP3 kaydını kaydet"
                             : L"CD kalitesinde WAV kaydını kaydet");
        DWORD options{};
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                               FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES);
        }
        const COMDLG_FILTERSPEC wavFilters[]{{L"WAV ses dosyası (*.wav)", L"*.wav"}};
        const COMDLG_FILTERSPEC mp3Filters[]{{L"MP3 ses dosyası (*.mp3)", L"*.mp3"}};
        const auto* filters = mp3 ? mp3Filters : wavFilters;
        dialog->SetFileTypes(1, filters);
        dialog->SetFileTypeIndex(1);
        dialog->SetDefaultExtension(mp3 ? L"mp3" : L"wav");
        std::wstring suggested = metadata.artist.empty() ? metadata.title
                                                          : metadata.artist + L" - " + metadata.title;
        suggested = neon::safeRecordingFileName(std::move(suggested)) + (mp3 ? L".mp3" : L".wav");
        dialog->SetFileName(suggested.c_str());

        ComPtr<IShellItem> musicFolder;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Music, KF_FLAG_DEFAULT, nullptr,
                                           IID_PPV_ARGS(&musicFolder)))) {
            dialog->SetDefaultFolder(musicFolder.Get());
        }
        result = dialog->Show(window_);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(result)) return {};
        ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item))) return {};
        PWSTR path{};
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
        std::filesystem::path destination(path);
        CoTaskMemFree(path);
        return destination;
    }

    void saveRecording(bool mp3) {
        if (!unsaved_) return;
        neon::RecordingMetadata metadata{controlText(title_), controlText(artist_),
                                          controlText(album_), controlText(year_)};
        if (metadata.title.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
            MessageBoxW(window_, L"Lütfen parça adını yazın.", L"Eksik bilgi",
                        MB_OK | MB_ICONINFORMATION);
            SetFocus(title_);
            return;
        }
        const auto destination = chooseDestination(metadata, mp3);
        if (destination.empty()) return;
        SetWindowTextW(status_, mp3 ? L"MP3 dosyası 320 kbps olarak kodlanıyor…"
                                    : L"WAV dosyası hazırlanıyor…");
        EnableWindow(saveWav_, FALSE);
        EnableWindow(saveMp3_, FALSE);
        UpdateWindow(window_);

        const auto snapshot = recorder_.snapshot();
        std::wstring error;
        const bool saved = mp3
            ? neon::writeHighQualityMp3(recorder_.temporaryPcmPath(), snapshot.sampleFrames,
                                        destination, metadata, error)
            : neon::writeCdQualityWave(recorder_.temporaryPcmPath(), snapshot.sampleFrames,
                                       destination, metadata, error);
        if (!saved) {
            EnableWindow(saveWav_, TRUE);
            EnableWindow(saveMp3_, TRUE);
            SetWindowTextW(status_, L"Dosya kaydedilemedi — yeniden deneyebilirsiniz");
            MessageBoxW(window_, error.c_str(), L"Neon Recorder", MB_OK | MB_ICONERROR);
            return;
        }
        recorder_.discard();
        unsaved_ = false;
        EnableWindow(record_, TRUE);
        EnableWindow(stop_, FALSE);
        EnableWindow(saveWav_, FALSE);
        EnableWindow(saveMp3_, FALSE);
        EnableWindow(newRecording_, TRUE);
        setMetadataFieldsState(automaticModeEnabled(), automaticModeEnabled());
        const std::wstring status = L"Kaydedildi: " + destination.wstring();
        SetWindowTextW(status_, status.c_str());
        MessageBoxW(window_, mp3 ? L"Sessiz uçlar kırpıldı; kayıt 320 kbps MP3 olarak kaydedildi."
                                 : L"Sessiz uçlar kırpıldı; kayıt CD kalitesinde WAV olarak kaydedildi.",
                    L"Kayıt tamamlandı", MB_OK | MB_ICONINFORMATION);
        if (automaticModeEnabled()) handleMediaSession(lastMedia_);
    }

    void resetRecording(bool ask) {
        if (ask && unsaved_) {
            const int answer = MessageBoxW(window_, L"Kaydedilmemiş kayıt silinsin mi?", L"Yeni kayıt",
                                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) return;
        }
        recorder_.discard();
        expectedRecording_ = false;
        unsaved_ = false;
        for (HWND control : {title_, artist_, album_, year_}) SetWindowTextW(control, L"");
        automaticArtwork_.clear();
        automaticArtworkMimeType_.clear();
        setArtworkStatus(false);
        SetWindowTextW(clock_, L"00:00:00");
        SetWindowTextW(status_, L"Kayıt için hazır");
        SendMessageW(leftLevel_, PBM_SETPOS, 0, 0);
        SendMessageW(rightLevel_, PBM_SETPOS, 0, 0);
        setIdleControls();
        InvalidateRect(window_, nullptr, FALSE);
        if (automaticModeEnabled()) handleMediaSession(lastMedia_);
    }

    HWND window_{};
    HWND header_{};
    HWND subtitle_{};
    HWND format_{};
    HWND status_{};
    HWND clock_{};
    HWND record_{};
    HWND stop_{};
    HWND leftLevel_{};
    HWND rightLevel_{};
    HWND autoMode_{};
    HWND autoSkip_{};
    HWND autoSource_{};
    HWND autoFormat_{};
    HWND autoFolderButton_{};
    HWND autoPath_{};
    HWND lastAutoSave_{};
    HWND metadataHeader_{};
    HWND titleLabel_{};
    HWND title_{};
    HWND artistLabel_{};
    HWND artist_{};
    HWND albumLabel_{};
    HWND album_{};
    HWND yearLabel_{};
    HWND year_{};
    HWND artworkStatus_{};
    HWND saveWav_{};
    HWND saveMp3_{};
    HWND newRecording_{};
    HWND autoNotice_{};
    HWND hint_{};
    HBRUSH backgroundBrush_{};
    HBRUSH editBrush_{};
    HFONT headerFont_{};
    HFONT sectionFont_{};
    HFONT normalFont_{};
    HFONT buttonFont_{};
    HFONT clockFont_{};
    neon::LoopbackRecorder recorder_;
    neon::MediaSessionWatcher mediaWatcher_;
    AutoSaveQueue autoSaveQueue_;
    neon::MediaSessionInfo lastMedia_;
    std::filesystem::path autoFolder_;
    std::wstring autoTrackKey_;
    std::wstring autoSkippedTrackKey_;
    std::unordered_map<std::wstring, std::uint64_t> pendingRecordingKeys_;
    std::vector<std::uint8_t> automaticArtwork_;
    std::string automaticArtworkMimeType_;
    std::uint64_t autoStopAt_{};
    std::uint64_t autoStartedAt_{};
    std::int64_t autoPositionTicks_{};
    std::int64_t autoEndTicks_{};
    std::uint64_t autoDurationTicks_{};
    bool expectedRecording_{};
    bool automaticCapture_{};
    bool automaticReplaceExisting_{};
    bool autoTimelineAvailable_{};
    bool unsaved_{};
};

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<RecorderWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_CREATE: {
            app = new RecorderWindow(window);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            if (!app->create()) return -1;
            return 0;
        }
        case WM_COMMAND:
            if (app && (HIWORD(wParam) == BN_CLICKED || HIWORD(wParam) == CBN_SELCHANGE)) {
                app->command(LOWORD(wParam));
            }
            return 0;
        case WM_TIMER:
            if (app && wParam == uiTimer) app->timer();
            return 0;
        case mediaChangedMessage:
            if (app) app->mediaChanged();
            return 0;
        case autoSaveCompletedMessage:
            if (app) app->autoSaveCompleted();
            return 0;
        case WM_PAINT:
            if (app) { app->paint(); return 0; }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            if (app) return app->colorControl(message, reinterpret_cast<HDC>(wParam),
                                               reinterpret_cast<HWND>(lParam));
            break;
        case WM_CLOSE:
            if (!app || app->canClose()) DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (app) {
                app->destroy();
                delete app;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            }
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        MessageBoxW(nullptr, L"Windows bileşenleri başlatılamadı.", L"Neon Recorder",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(applicationIcon));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = windowClassName;
    if (!RegisterClassExW(&windowClass)) {
        CoUninitialize();
        return 1;
    }

    RECT windowRect{0, 0, 754, 705};
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&windowRect, style, FALSE, 0);
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
    const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);
    HWND window = CreateWindowExW(0, windowClassName, L"Neon Recorder",
                                  style, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) {
        CoUninitialize();
        return 1;
    }
    constexpr BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(window, 20, &useDarkMode, sizeof(useDarkMode));
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
