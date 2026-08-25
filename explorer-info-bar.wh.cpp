// ==WindhawkMod==
// @id              explorer-info-bar
// @name            Explorer Info Bar
// @description     Enhances File Explorer's bottom info bar with drive, content, selection, and single-file details, with customizable styles and colors.
// @version         0.49
// @author          digart11
// @include         explorer.exe
// @compilerOptions -lole32 -lshell32 -luuid -lgdi32 -lcomctl32 -lpropsys
// ==/WindhawkMod==

// ==WindhawkModSettings==
/*
- style: simple
  $name: Style
  $description: Choose how the info bar sections are displayed.
  $options:
    - simple: Simple
    - panes: Flat panes
    - cards: Soft cards

- order: drive-content-selection
  $name: Section order
  $description: Choose the left-to-right order of Drive, Content and Selected.
  $options:
    - drive-content-selection: Drive / Content / Selected
    - drive-selection-content: Drive / Selected / Content
    - content-drive-selection: Content / Drive / Selected
    - content-selection-drive: Content / Selected / Drive
    - selection-drive-content: Selected / Drive / Content
    - selection-content-drive: Selected / Content / Drive

- showDrive: true
  $name: Show Drive
  $description: Show drive free-space information.

- showContent: true
  $name: Show Content
  $description: Show folder and file totals for the current location.

- showSelection: true
  $name: Show Selected
  $description: Show information about the current selection.

- singleFileDetails: true
  $name: Show Single File Details
  $description: Show file extension and details when possible.

- textColor: auto
  $name: Text color
  $description: "Use auto to inherit Explorer, or enter #RRGGBB."

- driveColor: auto
  $name: Drive panel color
  $description: "Flat panes/cards only. Use auto to derive from Explorer, or enter #RRGGBB."

- contentColor: auto
  $name: Content panel color
  $description: "Flat panes/cards only. Use auto to derive from Explorer, or enter #RRGGBB."

- selectionColor: auto
  $name: Selected panel color
  $description: "Flat panes/cards only. Use auto to derive from Explorer, or enter #RRGGBB."

- fileDetailsColor: auto
  $name: File Details panel color
  $description: "Flat panes/cards only. Use auto to derive from Explorer, or enter #RRGGBB."

- dividerColor: auto
  $name: Divider / border color (Soft Cards style only)
  $description: "Sets the Soft Cards border color. Use auto to derive from Explorer, or enter #RRGGBB."
*/
// ==/WindhawkModSettings==


#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <propkey.h>
#include <propvarutil.h>

#include <string>
#include <array>
#include <atomic>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <cstdlib>

#define CWM_GETISHELLBROWSER (WM_USER + 7)

static constexpr UINT kNativeStatusTextFormat = 0x00000824;
static constexpr int kStatusRowHeight = 24;
static constexpr ULONGLONG kStatusMarkerLifetimeMs = 250;
static constexpr DWORD kInitialRefreshDelayMs = 1000;
static constexpr DWORD kRefreshIntervalMs = 500;

// ============================================================
// DrawText hook
// ============================================================

using DrawTextW_t = int (WINAPI*)(
    HDC,
    LPCWSTR,
    int,
    LPRECT,
    UINT
);

static DrawTextW_t DrawTextW_Original = nullptr;

using BitBlt_t = BOOL (WINAPI*)(
    HDC,
    int,
    int,
    int,
    int,
    HDC,
    int,
    int,
    DWORD
);

static BitBlt_t BitBlt_Original = nullptr;

// Correlate Explorer's buffered status render with the final DirectUIHWND copy.
thread_local HDC g_statusSourceDc = nullptr;
thread_local ULONGLONG g_statusMarkTick = 0;
thread_local RECT g_statusRowRect{};
thread_local bool g_insideFinalPaint = false;

static std::atomic<HWND> g_lastDirectUiHwnd{nullptr};

enum class InfoBarStyle
{
    Simple,
    Panes,
    Cards
};

enum class InfoBarSection
{
    Drive,
    Content,
    Selection
};

struct ColorOverride
{
    bool enabled = false;
    COLORREF value = CLR_INVALID;
};

struct ModSettings
{
    InfoBarStyle style = InfoBarStyle::Simple;

    std::array<InfoBarSection, 3> sectionOrder
    {
        InfoBarSection::Drive,
        InfoBarSection::Content,
        InfoBarSection::Selection
    };

    ColorOverride textColor;
    ColorOverride driveColor;
    ColorOverride contentColor;
    ColorOverride selectionColor;
    ColorOverride fileDetailsColor;
    ColorOverride dividerColor;

    bool showDrive = true;
    bool showContent = true;
    bool showSelection = true;
    bool singleFileDetails = true;
};

static SRWLOCK g_settingsLock = SRWLOCK_INIT;
static ModSettings g_settings;

static COLORREF g_stableRowBackground = CLR_INVALID;
static COLORREF g_stableNativeTextColor = CLR_INVALID;
static HWND g_stableRowBackgroundHwnd = nullptr;
static constexpr UINT_PTR kDirectUiSubclassId = 0xE1B029;

static LRESULT CALLBACK DirectUiSubclassProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR refData
);

// ============================================================
// State
// ============================================================

static DWORD g_pid = 0;

static HANDLE g_workerThread = nullptr;
static HANDLE g_stopEvent = nullptr;

static CRITICAL_SECTION g_cacheLock;

static int g_selected = -1;
static std::wstring g_contentGroup = L"Loading...";
static std::wstring g_selectionGroup;
static std::wstring g_driveGroup;
static std::wstring g_fileDetailsGroup;


static std::wstring GetStringSetting(
    PCWSTR name
)
{
    PCWSTR raw =
        Wh_GetStringSetting(
            name
        );

    std::wstring value =
        raw ? raw : L"";

    if (raw)
        Wh_FreeStringSetting(raw);

    return value;
}

static ColorOverride ParseColorOverride(
    const std::wstring& value
)
{
    ColorOverride result;

    if (
        value.empty() ||
        _wcsicmp(
            value.c_str(),
            L"auto"
        ) == 0
    )
    {
        return result;
    }

    if (
        value.length() != 7 ||
        value[0] != L'#'
    )
    {
        return result;
    }

    wchar_t* end = nullptr;

    unsigned long rgb =
        wcstoul(
            value.c_str() + 1,
            &end,
            16
        );

    if (
        !end ||
        *end != L'\0' ||
        rgb > 0xFFFFFF
    )
    {
        return result;
    }

    result.enabled = true;
    result.value =
        RGB(
            (rgb >> 16) & 0xFF,
            (rgb >> 8) & 0xFF,
            rgb & 0xFF
        );

    return result;
}

static std::array<InfoBarSection, 3> ParseSectionOrder(
    const std::wstring& order
)
{
    if (order == L"drive-selection-content")
    {
        return
        {
            InfoBarSection::Drive,
            InfoBarSection::Selection,
            InfoBarSection::Content
        };
    }

    if (order == L"content-drive-selection")
    {
        return
        {
            InfoBarSection::Content,
            InfoBarSection::Drive,
            InfoBarSection::Selection
        };
    }

    if (order == L"content-selection-drive")
    {
        return
        {
            InfoBarSection::Content,
            InfoBarSection::Selection,
            InfoBarSection::Drive
        };
    }

    if (order == L"selection-drive-content")
    {
        return
        {
            InfoBarSection::Selection,
            InfoBarSection::Drive,
            InfoBarSection::Content
        };
    }

    if (order == L"selection-content-drive")
    {
        return
        {
            InfoBarSection::Selection,
            InfoBarSection::Content,
            InfoBarSection::Drive
        };
    }

    return
    {
        InfoBarSection::Drive,
        InfoBarSection::Content,
        InfoBarSection::Selection
    };
}

static void LoadSettings()
{
    ModSettings settings;

    const std::wstring style =
        GetStringSetting(
            L"style"
        );

    if (style == L"panes")
        settings.style = InfoBarStyle::Panes;
    else if (style == L"cards")
        settings.style = InfoBarStyle::Cards;

    settings.sectionOrder =
        ParseSectionOrder(
            GetStringSetting(
                L"order"
            )
        );

    settings.textColor =
        ParseColorOverride(
            GetStringSetting(
                L"textColor"
            )
        );

    settings.driveColor =
        ParseColorOverride(
            GetStringSetting(
                L"driveColor"
            )
        );

    settings.contentColor =
        ParseColorOverride(
            GetStringSetting(
                L"contentColor"
            )
        );

    settings.selectionColor =
        ParseColorOverride(
            GetStringSetting(
                L"selectionColor"
            )
        );

    settings.fileDetailsColor =
        ParseColorOverride(
            GetStringSetting(
                L"fileDetailsColor"
            )
        );

    settings.dividerColor =
        ParseColorOverride(
            GetStringSetting(
                L"dividerColor"
            )
        );

    settings.showDrive =
        Wh_GetIntSetting(
            L"showDrive"
        ) != 0;

    settings.showContent =
        Wh_GetIntSetting(
            L"showContent"
        ) != 0;

    settings.showSelection =
        Wh_GetIntSetting(
            L"showSelection"
        ) != 0;

    settings.singleFileDetails =
        Wh_GetIntSetting(
            L"singleFileDetails"
        ) != 0;

    AcquireSRWLockExclusive(
        &g_settingsLock
    );

    g_settings =
        settings;

    ReleaseSRWLockExclusive(
        &g_settingsLock
    );
}

static ModSettings GetSettingsSnapshot()
{
    AcquireSRWLockShared(
        &g_settingsLock
    );

    ModSettings settings =
        g_settings;

    ReleaseSRWLockShared(
        &g_settingsLock
    );

    return settings;
}

// ============================================================
// Helpers
// ============================================================

static bool ContainsItemToken(
    LPCWSTR text,
    int length
)
{
    if (
        !text ||
        length < 4
    )
    {
        return false;
    }

    for (
        int i = 0;
        i <= length - 4;
        i++
    )
    {
        if (
            towlower(text[i]) == L'i' &&
            towlower(text[i + 1]) == L't' &&
            towlower(text[i + 2]) == L'e' &&
            towlower(text[i + 3]) == L'm'
        )
        {
            return true;
        }
    }

    return false;
}

enum class ByteFormat
{
    Adaptive,
    OneDecimal
};

static std::wstring FormatBytes(
    ULONGLONG bytes,
    ByteFormat format = ByteFormat::Adaptive
)
{
    wchar_t buf[64] = {};

    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    constexpr double TB = GB * 1024.0;

    if (bytes >= static_cast<ULONGLONG>(TB))
    {
        swprintf(
            buf,
            ARRAYSIZE(buf),
            format == ByteFormat::OneDecimal
                ? L"%.1fTB"
                : L"%.2fTB",
            bytes / TB
        );
    }
    else if (bytes >= static_cast<ULONGLONG>(GB))
    {
        swprintf(
            buf,
            ARRAYSIZE(buf),
            format == ByteFormat::OneDecimal
                ? L"%.1fGB"
                : L"%.2fGB",
            bytes / GB
        );
    }
    else if (bytes >= static_cast<ULONGLONG>(MB))
    {
        const double mb =
            bytes / MB;

        if (format == ByteFormat::OneDecimal)
        {
            swprintf(
                buf,
                ARRAYSIZE(buf),
                L"%.1fMB",
                mb
            );
        }
        else if (mb >= 100.0)
        {
            swprintf(
                buf,
                ARRAYSIZE(buf),
                L"%.0fMB",
                mb
            );
        }
        else
        {
            swprintf(
                buf,
                ARRAYSIZE(buf),
                L"%.2fMB",
                mb
            );
        }
    }
    else if (bytes >= static_cast<ULONGLONG>(KB))
    {
        swprintf(
            buf,
            ARRAYSIZE(buf),
            L"%.0fKB",
            bytes / KB
        );
    }
    else
    {
        swprintf(
            buf,
            ARRAYSIZE(buf),
            L"%lluB",
            bytes
        );
    }

    return buf;
}


static std::wstring GetLiteralExtension(
    const std::wstring& path
)
{
    const size_t slash =
        path.find_last_of(
            L"\\/"
        );

    const size_t dot =
        path.find_last_of(
            L'.'
        );

    if (
        dot == std::wstring::npos ||
        dot + 1 >= path.length() ||
        (
            slash != std::wstring::npos &&
            dot < slash
        )
    )
    {
        return L"";
    }

    // Preserve the actual extension, including the dot and original case.
    return path.substr(
        dot
    );
}

static bool ReadUInt32Property(
    IShellItem2* item,
    REFPROPERTYKEY key,
    UINT32* value
)
{
    if (!item || !value)
        return false;

    PROPVARIANT property;
    PropVariantInit(
        &property
    );

    HRESULT hr =
        item->GetProperty(
            key,
            &property
        );

    ULONG convertedValue =
        0;

    if (SUCCEEDED(hr))
    {
        hr =
            PropVariantToUInt32(
                property,
                &convertedValue
            );
    }

    PropVariantClear(
        &property
    );

    if (FAILED(hr))
        return false;

    *value =
        static_cast<UINT32>(
            convertedValue
        );

    return true;
}

static bool ReadUInt64Property(
    IShellItem2* item,
    REFPROPERTYKEY key,
    ULONGLONG* value
)
{
    if (!item || !value)
        return false;

    PROPVARIANT property;
    PropVariantInit(
        &property
    );

    HRESULT hr =
        item->GetProperty(
            key,
            &property
        );

    if (SUCCEEDED(hr))
    {
        hr =
            PropVariantToUInt64(
                property,
                value
            );
    }

    PropVariantClear(
        &property
    );

    return SUCCEEDED(hr);
}

static std::wstring FormatMediaDuration(
    ULONGLONG duration100ns
)
{
    if (duration100ns == 0)
        return L"";

    const ULONGLONG totalSeconds =
        duration100ns /
        10000000ULL;

    const ULONGLONG hours =
        totalSeconds /
        3600ULL;

    const ULONGLONG minutes =
        (
            totalSeconds %
            3600ULL
        ) /
        60ULL;

    const ULONGLONG seconds =
        totalSeconds %
        60ULL;

    wchar_t buffer[64] = {};

    swprintf(
        buffer,
        ARRAYSIZE(buffer),
        L"%02llu:%02llu:%02llu",
        hours,
        minutes,
        seconds
    );

    return buffer;
}

static std::wstring BuildSingleFileDetails(
    IShellItem* item,
    const std::wstring& path
)
{
    if (!item || path.empty())
        return L"";

    const std::wstring extension =
        GetLiteralExtension(
            path
        );

    std::wstring result =
        extension.empty()
            ? L"no extension"
            : extension;

    IShellItem2* item2 =
        nullptr;

    if (
        FAILED(
            item->QueryInterface(
                IID_PPV_ARGS(
                    &item2
                )
            )
        ) ||
        !item2
    )
    {
        return result;
    }

    UINT32 imageWidth = 0;
    UINT32 imageHeight = 0;
    UINT32 videoWidth = 0;
    UINT32 videoHeight = 0;
    ULONGLONG duration = 0;

    const bool hasImageWidth =
        ReadUInt32Property(
            item2,
            PKEY_Image_HorizontalSize,
            &imageWidth
        );

    const bool hasImageHeight =
        ReadUInt32Property(
            item2,
            PKEY_Image_VerticalSize,
            &imageHeight
        );

    const bool hasVideoWidth =
        ReadUInt32Property(
            item2,
            PKEY_Video_FrameWidth,
            &videoWidth
        );

    const bool hasVideoHeight =
        ReadUInt32Property(
            item2,
            PKEY_Video_FrameHeight,
            &videoHeight
        );

    const bool hasDuration =
        ReadUInt64Property(
            item2,
            PKEY_Media_Duration,
            &duration
        );

    item2->Release();

    UINT32 width = 0;
    UINT32 height = 0;

    if (
        hasVideoWidth &&
        hasVideoHeight &&
        videoWidth > 0 &&
        videoHeight > 0
    )
    {
        width =
            videoWidth;

        height =
            videoHeight;
    }
    else if (
        hasImageWidth &&
        hasImageHeight &&
        imageWidth > 0 &&
        imageHeight > 0
    )
    {
        width =
            imageWidth;

        height =
            imageHeight;
    }

    const std::wstring durationText =
        (
            hasDuration &&
            duration > 0
        )
            ? FormatMediaDuration(
                duration
            )
            : L"";

    const bool hasDimensions =
        width > 0 &&
        height > 0;

    const bool hasMetadata =
        hasDimensions ||
        !durationText.empty();

    if (!hasMetadata)
        return result;

    result +=
        L" (";

    if (hasDimensions)
    {
        wchar_t dimensions[64] = {};

        swprintf(
            dimensions,
            ARRAYSIZE(dimensions),
            L"%u\x00D7%u",
            width,
            height
        );

        result +=
            dimensions;
    }

    if (!durationText.empty())
    {
        if (hasDimensions)
        {
            result +=
                L", ";
        }

        result +=
            durationText;
    }

    result +=
        L")";

    return result;
}

static bool GetFilesystemInfo(
    const wchar_t* path,
    bool* isDirectory,
    ULONGLONG* fileSize
)
{
    if (!path || !*path)
        return false;

    WIN32_FILE_ATTRIBUTE_DATA data{};

    if (!GetFileAttributesExW(
            path,
            GetFileExInfoStandard,
            &data))
    {
        return false;
    }

    bool directory =
        (data.dwFileAttributes &
         FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (isDirectory)
        *isDirectory = directory;

    if (fileSize)
    {
        if (directory)
        {
            *fileSize = 0;
        }
        else
        {
            *fileSize =
                (static_cast<ULONGLONG>(
                    data.nFileSizeHigh
                ) << 32) |
                data.nFileSizeLow;
        }
    }

    return true;
}

// ============================================================
// Explorer window discovery
// ============================================================

struct FindCabinetContext
{
    DWORD pid;
    HWND hwnd;
};

static BOOL CALLBACK FindCabinetProc(
    HWND hwnd,
    LPARAM lParam
)
{
    auto* ctx =
        reinterpret_cast<FindCabinetContext*>(
            lParam
        );

    DWORD pid = 0;

    GetWindowThreadProcessId(
        hwnd,
        &pid
    );

    if (pid != ctx->pid)
        return TRUE;

    wchar_t cls[128] = {};

    GetClassNameW(
        hwnd,
        cls,
        ARRAYSIZE(cls)
    );

    if (
        wcscmp(
            cls,
            L"CabinetWClass"
        ) == 0 &&
        IsWindowVisible(hwnd)
    )
    {
        ctx->hwnd = hwnd;
        return FALSE;
    }

    return TRUE;
}

static HWND FindCabinetWindow()
{
    FindCabinetContext ctx{
        g_pid,
        nullptr
    };

    EnumWindows(
        FindCabinetProc,
        reinterpret_cast<LPARAM>(
            &ctx
        )
    );

    return ctx.hwnd;
}

static IShellBrowser* TryGetShellBrowser(
    HWND hwnd
)
{
    if (!hwnd)
        return nullptr;

    auto* browser =
        reinterpret_cast<IShellBrowser*>(
            SendMessageW(
                hwnd,
                CWM_GETISHELLBROWSER,
                0,
                0
            )
        );

    if (browser)
        browser->AddRef();

    return browser;
}

// ============================================================
// Redraw
// ============================================================

static void RefreshInfoBar()
{
    HWND hwnd =
        g_lastDirectUiHwnd.load(
            std::memory_order_relaxed
        );

    if (!hwnd || !IsWindow(hwnd))
        return;

    RECT client{};

    if (!GetClientRect(hwnd, &client))
        return;

    RECT row =
        client;

    row.top =
        client.bottom > kStatusRowHeight
            ? client.bottom - kStatusRowHeight
            : 0;

    // Ask DirectUI to repaint only the bottom row.
    // Our subclass paints our info after DirectUI finishes its own WM_PAINT.
    InvalidateRect(
        hwnd,
        &row,
        FALSE
    );
}

// ============================================================
// Cache
// ============================================================

static void GetCachedGroups(
    std::wstring& contentGroup,
    std::wstring& selectionGroup,
    std::wstring& driveGroup,
    std::wstring& fileDetailsGroup,
    int& selected
)
{
    EnterCriticalSection(
        &g_cacheLock
    );

    contentGroup =
        g_contentGroup;

    selectionGroup =
        g_selectionGroup;

    driveGroup =
        g_driveGroup;

    fileDetailsGroup =
        g_fileDetailsGroup;

    selected =
        g_selected;

    LeaveCriticalSection(
        &g_cacheLock
    );
}

static bool UpdateCache(
    int files,
    int folders,

    int selected,
    int selectedFiles,
    int selectedFolders,

    ULONGLONG directFileBytes,
    ULONGLONG selectedBytes,

    ULONGLONG freeBytes,
    ULONGLONG driveTotalBytes,

    wchar_t driveLetter,

    const std::wstring& path,
    const std::wstring& fileDetailsText
)
{
    // --------------------------------------------------------
    // Drive
    // --------------------------------------------------------

    std::wstring driveText;

    if (
        driveLetter != L'?' &&
        driveTotalBytes > 0
    )
    {
        wchar_t driveBuf[256] = {};

        swprintf(
            driveBuf,
            ARRAYSIZE(driveBuf),
            L"Drive %c: %s free",
            driveLetter,
            FormatBytes(freeBytes, ByteFormat::OneDecimal).c_str()
        );

        driveText = driveBuf;
    }

    // --------------------------------------------------------
    // Content
    // --------------------------------------------------------

    wchar_t contentBuf[256] = {};

    swprintf(
        contentBuf,
        ARRAYSIZE(contentBuf),
        L"Content: %d folder%s / %d file%s (%s)",
        folders,
        folders == 1 ? L"" : L"s",
        files,
        files == 1 ? L"" : L"s",
        FormatBytes(directFileBytes).c_str()
    );

    std::wstring contentText =
        contentBuf;

    // --------------------------------------------------------
    // Selection
    // --------------------------------------------------------

    std::wstring selectionText;

    if (selected > 0)
    {
        wchar_t selectedBuf[256] = {};

        if (
            selectedFolders > 0 &&
            selectedFiles > 0
        )
        {
            swprintf(
                selectedBuf,
                ARRAYSIZE(selectedBuf),
                L"Selected: %d folder%s / %d file%s (%s)",
                selectedFolders,
                selectedFolders == 1 ? L"" : L"s",
                selectedFiles,
                selectedFiles == 1 ? L"" : L"s",
                FormatBytes(selectedBytes).c_str()
            );
        }
        else if (selectedFolders > 0)
        {
            swprintf(
                selectedBuf,
                ARRAYSIZE(selectedBuf),
                L"Selected: %d folder%s",
                selectedFolders,
                selectedFolders == 1 ? L"" : L"s"
            );
        }
        else
        {
            swprintf(
                selectedBuf,
                ARRAYSIZE(selectedBuf),
                L"Selected: %d file%s (%s)",
                selectedFiles,
                selectedFiles == 1 ? L"" : L"s",
                FormatBytes(selectedBytes).c_str()
            );
        }

        selectionText = selectedBuf;
    }

    bool changed = false;

    EnterCriticalSection(
        &g_cacheLock
    );

    if (
        contentText != g_contentGroup ||
        selectionText != g_selectionGroup ||
        driveText != g_driveGroup ||
        fileDetailsText != g_fileDetailsGroup ||
        selected != g_selected
    )
    {
        changed = true;

        g_selected =
            selected;

        g_contentGroup =
            contentText;

        g_selectionGroup =
            selectionText;

        g_driveGroup =
            driveText;

        g_fileDetailsGroup =
            fileDetailsText;
    }

    LeaveCriticalSection(
        &g_cacheLock
    );

    if (changed)
    {
        Wh_Log(
            L"INFOBAR path='%s' "
            L"folders=%d files=%d "
            L"selected=%d %dF/%dD "
            L"fileBytes=%llu selectedBytes=%llu "
            L"drive=%c free=%llu total=%llu",

            path.c_str(),

            folders,
            files,

            selected,
            selectedFiles,
            selectedFolders,

            directFileBytes,
            selectedBytes,

            driveLetter,
            freeBytes,
            driveTotalBytes
        );
    }

    return changed;
}

// ============================================================
// Read Explorer state
// ============================================================

static bool ReadCurrentView()
{
    HWND cabinet =
        FindCabinetWindow();

    if (!cabinet)
        return false;

    HWND tab =
        FindWindowExW(
            cabinet,
            nullptr,
            L"ShellTabWindowClass",
            nullptr
        );

    if (!tab)
        return false;

    IShellBrowser* browser =
        TryGetShellBrowser(
            tab
        );

    if (!browser)
        return false;

    IShellView* shellView =
        nullptr;

    HRESULT hr =
        browser->QueryActiveShellView(
            &shellView
        );

    if (
        FAILED(hr) ||
        !shellView
    )
    {
        browser->Release();

        return false;
    }

    IFolderView2* folderView =
        nullptr;

    hr =
        shellView->QueryInterface(
            IID_PPV_ARGS(
                &folderView
            )
        );

    if (
        FAILED(hr) ||
        !folderView
    )
    {
        shellView->Release();
        browser->Release();

        return false;
    }

    // ========================================================
    // Current folder path
    // ========================================================

    std::wstring currentPath;

    IShellItem* folderItem =
        nullptr;

    hr =
        folderView->GetFolder(
            IID_PPV_ARGS(
                &folderItem
            )
        );

    if (
        SUCCEEDED(hr) &&
        folderItem
    )
    {
        PWSTR path =
            nullptr;

        if (
            SUCCEEDED(
                folderItem->GetDisplayName(
                    SIGDN_FILESYSPATH,
                    &path
                )
            ) &&
            path
        )
        {
            currentPath =
                path;

            CoTaskMemFree(
                path
            );
        }

        folderItem->Release();
    }

    // ========================================================
    // Drive
    // ========================================================

    ULONGLONG freeBytes =
        0;

    ULONGLONG driveTotalBytes =
        0;

    wchar_t driveLetter =
        L'?';

    if (!currentPath.empty())
    {
        ULARGE_INTEGER freeAvailable{};
        ULARGE_INTEGER totalBytes{};
        ULARGE_INTEGER totalFree{};

        if (
            GetDiskFreeSpaceExW(
                currentPath.c_str(),
                &freeAvailable,
                &totalBytes,
                &totalFree
            )
        )
        {
            freeBytes =
                freeAvailable.QuadPart;

            driveTotalBytes =
                totalBytes.QuadPart;
        }

        if (
            currentPath.length() >= 2 &&
            currentPath[1] == L':'
        )
        {
            driveLetter =
                static_cast<wchar_t>(
                    towupper(
                        currentPath[0]
                    )
                );
        }
    }

    // ========================================================
    // Shell folder
    // ========================================================

    IShellFolder* shellFolder =
        nullptr;

    hr =
        folderView->GetFolder(
            IID_PPV_ARGS(
                &shellFolder
            )
        );

    if (
        FAILED(hr) ||
        !shellFolder
    )
    {
        folderView->Release();
        shellView->Release();
        browser->Release();

        return false;
    }

    // ========================================================
    // Immediate directory contents
    // ========================================================

    int total =
        0;

    hr =
        folderView->ItemCount(
            SVGIO_ALLVIEW,
            &total
        );

    int files =
        0;

    int folders =
        0;

    ULONGLONG directFileBytes =
        0;

    if (SUCCEEDED(hr))
    {
        for (
            int i = 0;
            i < total;
            i++
        )
        {
            PITEMID_CHILD pidl =
                nullptr;

            if (
                FAILED(
                    folderView->Item(
                        i,
                        &pidl
                    )
                ) ||
                !pidl
            )
            {
                continue;
            }

            IShellItem* item =
                nullptr;

            hr =
                SHCreateItemWithParent(
                    nullptr,
                    shellFolder,
                    pidl,
                    IID_PPV_ARGS(
                        &item
                    )
                );

            if (
                SUCCEEDED(hr) &&
                item
            )
            {
                PWSTR path =
                    nullptr;

                if (
                    SUCCEEDED(
                        item->GetDisplayName(
                            SIGDN_FILESYSPATH,
                            &path
                        )
                    ) &&
                    path
                )
                {
                    bool directory =
                        false;

                    ULONGLONG size =
                        0;

                    if (
                        GetFilesystemInfo(
                            path,
                            &directory,
                            &size
                        )
                    )
                    {
                        if (directory)
                        {
                            folders++;
                        }
                        else
                        {
                            files++;

                            directFileBytes +=
                                size;
                        }
                    }

                    CoTaskMemFree(
                        path
                    );
                }

                item->Release();
            }

            CoTaskMemFree(
                pidl
            );
        }
    }

    // ========================================================
    // Selection
    // ========================================================

    int selected =
        0;

    int selectedFiles =
        0;

    int selectedFolders =
        0;

    ULONGLONG selectedBytes =
        0;

    std::wstring singleFileDetails;

    const ModSettings settings =
        GetSettingsSnapshot();

    IShellItemArray* selection =
        nullptr;

    hr =
        folderView->GetSelection(
            FALSE,
            &selection
        );

    if (
        SUCCEEDED(hr) &&
        selection
    )
    {
        DWORD selectionCount =
            0;

        if (
            SUCCEEDED(
                selection->GetCount(
                    &selectionCount
                )
            )
        )
        {
            selected =
                static_cast<int>(
                    selectionCount
                );

            for (
                DWORD i = 0;
                i < selectionCount;
                i++
            )
            {
                IShellItem* item =
                    nullptr;

                if (
                    FAILED(
                        selection->GetItemAt(
                            i,
                            &item
                        )
                    ) ||
                    !item
                )
                {
                    continue;
                }

                PWSTR path =
                    nullptr;

                if (
                    SUCCEEDED(
                        item->GetDisplayName(
                            SIGDN_FILESYSPATH,
                            &path
                        )
                    ) &&
                    path
                )
                {
                    bool directory =
                        false;

                    ULONGLONG size =
                        0;

                    if (
                        GetFilesystemInfo(
                            path,
                            &directory,
                            &size
                        )
                    )
                    {
                        if (directory)
                        {
                            selectedFolders++;
                        }
                        else
                        {
                            selectedFiles++;

                            selectedBytes +=
                                size;

                            if (
                                selectionCount == 1
                            )
                            {
                                if (
                                    settings.singleFileDetails
                                )
                                {
                                    singleFileDetails =
                                        BuildSingleFileDetails(
                                            item,
                                            path
                                        );
                                }
                            }
                        }
                    }

                    CoTaskMemFree(
                        path
                    );
                }

                item->Release();
            }
        }

        selection->Release();
    }

    bool changed =
        UpdateCache(
            files,
            folders,

            selected,
            selectedFiles,
            selectedFolders,

            directFileBytes,
            selectedBytes,

            freeBytes,
            driveTotalBytes,

            driveLetter,

            currentPath,
            singleFileDetails
        );

    shellFolder->Release();
    folderView->Release();
    shellView->Release();
    browser->Release();

    return changed;
}

// ============================================================
// Worker
// ============================================================

static DWORD WINAPI WorkerThreadProc(
    LPVOID
)
{
    const HRESULT comHr =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED
        );

    Wh_Log(
        L"Explorer Info Bar worker start "
        L"PID=%lu COM=0x%08X",
        g_pid,
        static_cast<unsigned>(
            comHr
        )
    );

    if (
        WaitForSingleObject(
            g_stopEvent,
            kInitialRefreshDelayMs
        ) == WAIT_OBJECT_0
    )
    {
        if (SUCCEEDED(comHr))
            CoUninitialize();

        return 0;
    }

    while (true)
    {
        if (ReadCurrentView())
            RefreshInfoBar();

        if (
            WaitForSingleObject(
                g_stopEvent,
                kRefreshIntervalMs
            ) == WAIT_OBJECT_0
        )
        {
            break;
        }
    }

    if (SUCCEEDED(comHr))
        CoUninitialize();

    Wh_Log(
        L"Explorer Info Bar worker end PID=%lu",
        g_pid
    );

    return 0;
}

// ============================================================
// Final-paint renderer
// ============================================================

static bool IsDirectUiWindow(
    HWND hwnd
)
{
    if (!hwnd)
        return false;

    wchar_t cls[128] = {};

    if (!GetClassNameW(
            hwnd,
            cls,
            ARRAYSIZE(cls)))
    {
        return false;
    }

    return wcscmp(
        cls,
        L"DirectUIHWND"
    ) == 0;
}

static bool StatusMarkIsFresh(
    HDC source
)
{
    if (!g_statusSourceDc)
        return false;

    ULONGLONG now =
        GetTickCount64();

    if (
        now - g_statusMarkTick >
        kStatusMarkerLifetimeMs
    )
    {
        g_statusSourceDc = nullptr;
        return false;
    }

    return source ==
        g_statusSourceDc;
}

static int MeasureTextWidth(
    HDC hdc,
    const std::wstring& text
)
{
    if (text.empty())
        return 0;

    SIZE size{};

    if (!GetTextExtentPoint32W(
            hdc,
            text.c_str(),
            static_cast<int>(
                text.length()
            ),
            &size))
    {
        return 0;
    }

    return size.cx;
}

static int MeasureGapWidth(
    HDC hdc
)
{
    const wchar_t* gap =
        L"     ";

    SIZE size{};

    if (
        GetTextExtentPoint32W(
            hdc,
            gap,
            5,
            &size
        )
    )
    {
        return size.cx;
    }

    return 26;
}

static COLORREF PickBackgroundColor(
    HDC hdc,
    HWND hwnd,
    const RECT& row,
    int selected
)
{
    // Keep the native unselected status-row background stable.
    // Explorer can temporarily tint the row while items are selected,
    // which should not redefine the mod's automatic theme colors.

    if (
        selected > 0 &&
        hwnd == g_stableRowBackgroundHwnd &&
        g_stableRowBackground != CLR_INVALID
    )
    {
        return g_stableRowBackground;
    }

    const int y =
        row.top +
        ((row.bottom - row.top) / 2);

    const int samples[] =
    {
        420,
        520,
        620,
        720
    };

    COLORREF chosen =
        CLR_INVALID;

    for (int x : samples)
    {
        if (x >= row.right)
            continue;

        COLORREF c =
            GetPixel(
                hdc,
                x,
                y
            );

        if (c != CLR_INVALID)
        {
            chosen = c;
            break;
        }
    }

    if (chosen == CLR_INVALID)
    {
        if (
            hwnd == g_stableRowBackgroundHwnd &&
            g_stableRowBackground != CLR_INVALID
        )
        {
            return g_stableRowBackground;
        }

        chosen =
            RGB(
                32,
                32,
                32
            );
    }

    // Only learn/update the "normal" row background while unselected.
    if (selected <= 0)
    {
        g_stableRowBackgroundHwnd =
            hwnd;

        g_stableRowBackground =
            chosen;
    }

    return chosen;
}

static void DrawFinalPiece(
    HDC hdc,
    int& x,
    const RECT& row,
    const std::wstring& text,
    COLORREF color
)
{
    if (text.empty())
        return;

    int width =
        MeasureTextWidth(
            hdc,
            text
        );

    RECT rc =
        row;

    rc.left =
        x;

    rc.right =
        x + width + 4;

    SetTextColor(
        hdc,
        color
    );

    DrawTextW_Original(
        hdc,
        text.c_str(),
        -1,
        &rc,
        DT_SINGLELINE |
        DT_VCENTER |
        DT_NOPREFIX
    );

    x += width;
}

static void DrawFinalSeparator(
    HDC hdc,
    int& x,
    const RECT& row,
    COLORREF color
)
{
    x +=
        MeasureGapWidth(
            hdc
        );

    DrawFinalPiece(
        hdc,
        x,
        row,
        L"\x00B7",
        color
    );

    x +=
        MeasureGapWidth(
            hdc
        );
}

static int ColorLuminance(
    COLORREF color
)
{
    return (
        GetRValue(color) * 30 +
        GetGValue(color) * 59 +
        GetBValue(color) * 11
    ) / 100;
}

static COLORREF BlendColor(
    COLORREF base,
    COLORREF target,
    int targetPercent
)
{
    targetPercent =
        std::max(
            0,
            std::min(
                100,
                targetPercent
            )
        );

    const int basePercent =
        100 - targetPercent;

    return RGB(
        (
            GetRValue(base) * basePercent +
            GetRValue(target) * targetPercent
        ) / 100,
        (
            GetGValue(base) * basePercent +
            GetGValue(target) * targetPercent
        ) / 100,
        (
            GetBValue(base) * basePercent +
            GetBValue(target) * targetPercent
        ) / 100
    );
}

static COLORREF GetContrastingTextColor(
    COLORREF background
)
{
    return ColorLuminance(background) >= 140
        ? RGB(32, 32, 32)
        : RGB(232, 232, 232);
}

static COLORREF PickNativeTextColor(
    HDC hdc,
    HWND hwnd,
    COLORREF background,
    int selected
)
{
    if (
        selected > 0 &&
        hwnd == g_stableRowBackgroundHwnd &&
        g_stableNativeTextColor != CLR_INVALID
    )
    {
        return g_stableNativeTextColor;
    }

    COLORREF candidate =
        GetTextColor(
            hdc
        );

    if (
        candidate == CLR_INVALID ||
        std::abs(
            ColorLuminance(candidate) -
            ColorLuminance(background)
        ) < 70
    )
    {
        candidate =
            GetContrastingTextColor(
                background
            );
    }

    if (
        selected <= 0 &&
        hwnd == g_stableRowBackgroundHwnd
    )
    {
        g_stableNativeTextColor =
            candidate;
    }

    return candidate;
}

static COLORREF ResolveColor(
    const ColorOverride& colorOverride,
    COLORREF automaticColor
)
{
    return colorOverride.enabled
        ? colorOverride.value
        : automaticColor;
}

static void PaintFinalInfoBar(
    HDC hdc,
    HWND hwnd
)
{
    if (!hdc || !hwnd)
        return;

    RECT client{};

    if (!GetClientRect(
            hwnd,
            &client))
    {
        return;
    }

    RECT row =
        g_statusRowRect;

    // Fall back to Explorer's bottom status-row height if the native rect is unavailable.
    if (
        row.bottom <= row.top ||
        row.top < 0 ||
        row.bottom > client.bottom + 2
    )
    {
        row.top =
            client.bottom > kStatusRowHeight
                ? client.bottom - kStatusRowHeight
                : 0;

        row.bottom =
            client.bottom;
    }

    row.left = 6;

    // Preserve Explorer's right-side controls.
    row.right =
        client.right > 220
            ? client.right - 220
            : client.right;

    std::wstring contentGroup;
    std::wstring selectionGroup;
    std::wstring driveGroup;
    std::wstring fileDetailsGroup;

    int selected = 0;

    GetCachedGroups(
        contentGroup,
        selectionGroup,
        driveGroup,
        fileDetailsGroup,
        selected
    );

    const ModSettings settings =
        GetSettingsSnapshot();

    COLORREF background =
        PickBackgroundColor(
            hdc,
            hwnd,
            row,
            selected
        );

    HBRUSH brush =
        CreateSolidBrush(
            background
        );

    if (brush)
    {
        FillRect(
            hdc,
            &row,
            brush
        );

        DeleteObject(
            brush
        );
    }

    int oldBkMode =
        SetBkMode(
            hdc,
            TRANSPARENT
        );

    COLORREF oldTextColor =
        GetTextColor(
            hdc
        );

    // Match Explorer's native info-bar font metrics.
    HFONT font =
        CreateFontW(
            -12,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH,
            L"Segoe UI"
        );

    HFONT oldFont = nullptr;

    if (font)
    {
        oldFont =
            reinterpret_cast<HFONT>(
                SelectObject(
                    hdc,
                    font
                )
            );
    }

    const COLORREF nativeText =
        PickNativeTextColor(
            hdc,
            hwnd,
            background,
            selected
        );

    const COLORREF textColor =
        ResolveColor(
            settings.textColor,
            nativeText
        );

    // Auto mode must stay visually neutral and theme-compatible.
    // All three panels use the SAME theme-derived fill by default.
    // Individual sections differ only when the user explicitly overrides
    // Drive / Content / Selected colors in settings.
    const COLORREF automaticPanelColor =
        BlendColor(
            background,
            nativeText,
            6
        );

    const COLORREF drivePanelColor =
        ResolveColor(
            settings.driveColor,
            automaticPanelColor
        );

    const COLORREF contentPanelColor =
        ResolveColor(
            settings.contentColor,
            automaticPanelColor
        );

    const COLORREF selectionPanelColor =
        ResolveColor(
            settings.selectionColor,
            automaticPanelColor
        );

    const COLORREF fileDetailsPanelColor =
        ResolveColor(
            settings.fileDetailsColor,
            automaticPanelColor
        );

    const COLORREF automaticDividerColor =
        BlendColor(
            background,
            nativeText,
            22
        );

    // The custom divider/border override intentionally applies only to
    // Soft Cards. Simple and Flat Panes always derive their structure
    // from Explorer so changing this setting has no hidden side effects.
    const COLORREF cardBorderColor =
        ResolveColor(
            settings.dividerColor,
            automaticDividerColor
        );

    auto GetSectionText =
        [&](InfoBarSection section) -> const std::wstring&
        {
            if (section == InfoBarSection::Drive)
                return driveGroup;

            if (section == InfoBarSection::Content)
                return contentGroup;

            return selectionGroup;
        };

    auto IsSectionEnabled =
        [&](InfoBarSection section) -> bool
        {
            if (section == InfoBarSection::Drive)
                return settings.showDrive;

            if (section == InfoBarSection::Content)
                return settings.showContent;

            return settings.showSelection;
        };

    const bool showFileDetails =
        settings.singleFileDetails &&
        !fileDetailsGroup.empty();

    const bool hasVisibleContent =
        (
            settings.showDrive &&
            !driveGroup.empty()
        ) ||
        (
            settings.showContent &&
            !contentGroup.empty()
        ) ||
        (
            settings.showSelection &&
            !selectionGroup.empty()
        ) ||
        showFileDetails;

    // If every custom section is disabled or empty, leave Explorer's
    // native status bar untouched instead of painting an empty row.
    if (!hasVisibleContent)
        return;

    int x =
        14;

    bool drew =
        false;

    if (settings.style == InfoBarStyle::Simple)
    {
        for (InfoBarSection section : settings.sectionOrder)
        {
            if (!IsSectionEnabled(section))
                continue;

            const std::wstring& value =
                GetSectionText(
                    section
                );

            if (value.empty())
                continue;

            if (drew)
            {
                DrawFinalSeparator(
                    hdc,
                    x,
                    row,
                    automaticDividerColor
                );
            }

            DrawFinalPiece(
                hdc,
                x,
                row,
                value,
                textColor
            );

            drew =
                true;
        }

        if (showFileDetails)
        {
            if (drew)
            {
                DrawFinalSeparator(
                    hdc,
                    x,
                    row,
                    automaticDividerColor
                );
            }

            DrawFinalPiece(
                hdc,
                x,
                row,
                fileDetailsGroup,
                textColor
            );
        }
    }
    else
    {
        const bool cards =
            settings.style == InfoBarStyle::Cards;

        const int padX =
            cards ? 10 : 12;

        const int gap =
            cards ? 8 : 6;

        // Flat panes should begin flush with the left edge.
        // Cards keep a tiny 2 px inset so the rounded border isn't clipped.
        int paneX =
            cards ? 2 : 0;

        auto DrawBox =
            [&](const std::wstring& value,
                COLORREF fill,
                COLORREF border,
                COLORREF textColor)
        {
            if (value.empty())
                return;

            int width =
                MeasureTextWidth(
                    hdc,
                    value
                ) +
                padX * 2;

            RECT box{
                paneX,
                row.top + (cards ? 3 : 1),
                paneX + width,
                row.bottom - (cards ? 3 : 1)
            };

            if (box.right > row.right)
                box.right = row.right;

            if (cards)
            {
                HBRUSH fillBrush =
                    CreateSolidBrush(fill);

                HPEN pen =
                    CreatePen(
                        PS_SOLID,
                        1,
                        border
                    );

                if (fillBrush && pen)
                {
                    HBRUSH oldBrush =
                        reinterpret_cast<HBRUSH>(
                            SelectObject(hdc, fillBrush)
                        );

                    HPEN oldPen =
                        reinterpret_cast<HPEN>(
                            SelectObject(hdc, pen)
                        );

                    RoundRect(
                        hdc,
                        box.left,
                        box.top,
                        box.right,
                        box.bottom,
                        6,
                        6
                    );

                    SelectObject(hdc, oldPen);
                    SelectObject(hdc, oldBrush);
                }

                if (pen)
                    DeleteObject(pen);

                if (fillBrush)
                    DeleteObject(fillBrush);
            }
            else
            {
                HBRUSH fillBrush =
                    CreateSolidBrush(fill);

                if (fillBrush)
                {
                    FillRect(
                        hdc,
                        &box,
                        fillBrush
                    );

                    DeleteObject(fillBrush);
                }
            }

            RECT textRect =
                box;

            textRect.left += padX;
            textRect.right -= padX;

            SetTextColor(
                hdc,
                textColor
            );

            DrawTextW_Original(
                hdc,
                value.c_str(),
                -1,
                &textRect,
                DT_SINGLELINE |
                DT_VCENTER |
                DT_NOPREFIX |
                DT_END_ELLIPSIS
            );

            paneX =
                box.right +
                gap;
        };

        for (InfoBarSection section : settings.sectionOrder)
        {
            if (!IsSectionEnabled(section))
                continue;

            const std::wstring& value =
                GetSectionText(
                    section
                );

            if (value.empty())
                continue;

            COLORREF panelColor =
                contentPanelColor;

            if (section == InfoBarSection::Drive)
                panelColor = drivePanelColor;
            else if (section == InfoBarSection::Selection)
                panelColor = selectionPanelColor;

            DrawBox(
                value,
                panelColor,
                cardBorderColor,
                textColor
            );
        }

        if (showFileDetails)
        {
            DrawBox(
                fileDetailsGroup,
                fileDetailsPanelColor,
                cardBorderColor,
                textColor
            );
        }
    }

    if (oldFont)
    {
        SelectObject(
            hdc,
            oldFont
        );
    }

    if (font)
    {
        DeleteObject(
            font
        );
    }

    SetTextColor(
        hdc,
        oldTextColor
    );

    SetBkMode(
        hdc,
        oldBkMode
    );
}


static bool EnsureDirectUiSubclass(
    HWND hwnd
)
{
    if (!hwnd)
        return false;

    DWORD hwndThread =
        GetWindowThreadProcessId(
            hwnd,
            nullptr
        );

    if (
        hwndThread !=
        GetCurrentThreadId()
    )
    {
        Wh_Log(
            L"DirectUI subclass skipped: wrong thread hwnd=%p hwndTid=%lu currentTid=%lu",
            hwnd,
            hwndThread,
            GetCurrentThreadId()
        );

        return false;
    }

    if (
        GetWindowSubclass(
            hwnd,
            DirectUiSubclassProc,
            kDirectUiSubclassId,
            nullptr
        )
    )
    {
        return true;
    }

    if (
        !SetWindowSubclass(
            hwnd,
            DirectUiSubclassProc,
            kDirectUiSubclassId,
            0
        )
    )
    {
        Wh_Log(
            L"SetWindowSubclass failed hwnd=%p error=%lu",
            hwnd,
            GetLastError()
        );

        return false;
    }

    Wh_Log(
        L"DirectUI subclass installed hwnd=%p tid=%lu",
        hwnd,
        GetCurrentThreadId()
    );

    return true;
}

static LRESULT CALLBACK DirectUiSubclassProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR subclassId,
    DWORD_PTR refData
)
{
    if (msg == WM_NCDESTROY)
    {
        RemoveWindowSubclass(
            hwnd,
            DirectUiSubclassProc,
            kDirectUiSubclassId
        );

        if (
            g_lastDirectUiHwnd.load(
                std::memory_order_relaxed
            ) == hwnd
        )
        {
            g_lastDirectUiHwnd.store(
                nullptr,
                std::memory_order_relaxed
            );
        }

        if (g_stableRowBackgroundHwnd == hwnd)
        {
            g_stableRowBackgroundHwnd = nullptr;
            g_stableRowBackground = CLR_INVALID;
            g_stableNativeTextColor = CLR_INVALID;
        }

        return DefSubclassProc(
            hwnd,
            msg,
            wParam,
            lParam
        );
    }

    if (msg == WM_PAINT)
    {
        // Let DirectUI finish ALL of its own buffered painting first.
        LRESULT result =
            DefSubclassProc(
                hwnd,
                msg,
                wParam,
                lParam
            );

        HDC hdc =
            GetDC(hwnd);

        if (hdc)
        {
            g_insideFinalPaint =
                true;

            PaintFinalInfoBar(
                hdc,
                hwnd
            );

            g_insideFinalPaint =
                false;

            ReleaseDC(
                hwnd,
                hdc
            );
        }

        return result;
    }

    return DefSubclassProc(
        hwnd,
        msg,
        wParam,
        lParam
    );
}

// ============================================================
// DrawText marker hook
// ============================================================

static int WINAPI DrawTextW_Hook(
    HDC hdc,
    LPCWSTR text,
    int count,
    LPRECT rect,
    UINT format
)
{
    if (!DrawTextW_Original)
        return 0;

    if (text)
    {
        int length =
            count;

        if (length < 0)
        {
            length =
                static_cast<int>(
                    wcslen(text)
                );
        }

        if (
            length > 0 &&
            length < 512
        )
        {
            if (
                format == kNativeStatusTextFormat &&
                ContainsItemToken(
                    text,
                    length
                )
            )
            {
                g_statusSourceDc =
                    hdc;

                g_statusMarkTick =
                    GetTickCount64();

                if (rect)
                    g_statusRowRect = *rect;
            }
        }
    }

    // Important: do not modify the off-screen DirectUI render.
    return DrawTextW_Original(
        hdc,
        text,
        count,
        rect,
        format
    );
}

// ============================================================
// Final DirectUI copy hook
// ============================================================

static BOOL WINAPI BitBlt_Hook(
    HDC hdcDest,
    int xDest,
    int yDest,
    int width,
    int height,
    HDC hdcSrc,
    int xSrc,
    int ySrc,
    DWORD rop
)
{
    bool relevant =
        !g_insideFinalPaint &&
        StatusMarkIsFresh(
            hdcSrc
        );

    BOOL result =
        BitBlt_Original(
            hdcDest,
            xDest,
            yDest,
            width,
            height,
            hdcSrc,
            xSrc,
            ySrc,
            rop
        );

    if (!relevant)
        return result;

    HWND hwnd =
        WindowFromDC(
            hdcDest
        );

    if (!IsDirectUiWindow(hwnd))
        return result;

    g_lastDirectUiHwnd.store(
        hwnd,
        std::memory_order_relaxed
    );

    // This hook runs on DirectUI's UI thread, which is required for subclassing.
    // Install the subclass here so every future WM_PAINT ends with our row.
    const bool subclassed =
        EnsureDirectUiSubclass(
            hwnd
        );

    // The subclass cannot retroactively catch the WM_PAINT that is already
    // in progress when it is first installed, so finish this current frame
    // once directly after the relevant BitBlt.
    if (subclassed)
    {
        g_insideFinalPaint =
            true;

        PaintFinalInfoBar(
            hdcDest,
            hwnd
        );

        g_insideFinalPaint =
            false;
    }

    return result;
}


// ============================================================
// Windhawk lifecycle
// ============================================================

BOOL Wh_ModInit()
{
    g_pid =
        GetCurrentProcessId();

    Wh_Log(
        L"========== Explorer Info Bar 0.49 INIT PID=%lu ==========",
        g_pid
    );

    InitializeCriticalSection(
        &g_cacheLock
    );

    LoadSettings();

    HMODULE user32 =
        GetModuleHandleW(
            L"user32.dll"
        );

    HMODULE gdi32 =
        GetModuleHandleW(
            L"gdi32.dll"
        );

    if (!user32 || !gdi32)
    {
        Wh_Log(
            L"required system module not loaded"
        );

        DeleteCriticalSection(
            &g_cacheLock
        );

        return FALSE;
    }

    void* drawTextTarget =
        reinterpret_cast<void*>(
            GetProcAddress(
                user32,
                "DrawTextW"
            )
        );

    void* bitBltTarget =
        reinterpret_cast<void*>(
            GetProcAddress(
                gdi32,
                "BitBlt"
            )
        );

    if (!drawTextTarget || !bitBltTarget)
    {
        Wh_Log(
            L"required GDI targets not found"
        );

        DeleteCriticalSection(
            &g_cacheLock
        );

        return FALSE;
    }

    if (
        !Wh_SetFunctionHook(
            drawTextTarget,
            reinterpret_cast<void*>(
                DrawTextW_Hook
            ),
            reinterpret_cast<void**>(
                &DrawTextW_Original
            )
        )
    )
    {
        Wh_Log(
            L"DrawTextW hook failed"
        );

        DeleteCriticalSection(
            &g_cacheLock
        );

        return FALSE;
    }

    if (
        !Wh_SetFunctionHook(
            bitBltTarget,
            reinterpret_cast<void*>(
                BitBlt_Hook
            ),
            reinterpret_cast<void**>(
                &BitBlt_Original
            )
        )
    )
    {
        Wh_Log(
            L"BitBlt hook failed"
        );

        DeleteCriticalSection(
            &g_cacheLock
        );

        return FALSE;
    }

    g_stopEvent =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            nullptr
        );

    if (!g_stopEvent)
    {
        Wh_Log(
            L"stop event creation failed error=%lu",
            GetLastError()
        );

        DeleteCriticalSection(
            &g_cacheLock
        );

        return FALSE;
    }

    g_workerThread =
        CreateThread(
            nullptr,
            0,
            WorkerThreadProc,
            nullptr,
            0,
            nullptr
        );

    if (!g_workerThread)
    {
        Wh_Log(
            L"worker creation failed error=%lu",
            GetLastError()
        );

        CloseHandle(
            g_stopEvent
        );

        g_stopEvent =
            nullptr;

        DeleteCriticalSection(
            &g_cacheLock
        );

        return FALSE;
    }

    Wh_Log(
        L"Explorer Info Bar 0.49 ready"
    );

    return TRUE;
}


void Wh_ModSettingsChanged()
{
    LoadSettings();
    RefreshInfoBar();
}

void Wh_ModUninit()
{
    if (g_stopEvent)
    {
        SetEvent(
            g_stopEvent
        );
    }

    if (g_workerThread)
    {
        // The cache and COM state must outlive the worker. Wait until the
        // worker has fully stopped before releasing shared resources.
        WaitForSingleObject(
            g_workerThread,
            INFINITE
        );

        CloseHandle(
            g_workerThread
        );

        g_workerThread =
            nullptr;
    }

    if (g_stopEvent)
    {
        CloseHandle(
            g_stopEvent
        );

        g_stopEvent =
            nullptr;
    }

    DeleteCriticalSection(
        &g_cacheLock
    );

    Wh_Log(
        L"========== Explorer Info Bar 0.49 UNINIT PID=%lu ==========",
        g_pid
    );
}
