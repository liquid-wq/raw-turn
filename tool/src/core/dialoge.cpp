#include "core/dialoge.h"

#include <windows.h>
#include <shlobj.h>
#include <commdlg.h>

#include <cstring>

namespace core {
namespace {

int CALLBACK StartSetzen(HWND hwnd, UINT msg, LPARAM, LPARAM daten) {
    if (msg == BFFM_INITIALIZED && daten)
        ::SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, daten);
    return 0;
}

}  // namespace

std::string OrdnerWaehlen(const char* titel, const std::string& start) {
    char pfad[MAX_PATH] = {};
    BROWSEINFOA bi = {};
    bi.lpszTitle = titel;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    bi.lpfn = StartSetzen;
    bi.lParam = start.empty() ? 0 : (LPARAM)start.c_str();

    LPITEMIDLIST id = ::SHBrowseForFolderA(&bi);
    if (!id) return {};
    const bool ok = ::SHGetPathFromIDListA(id, pfad) != FALSE;
    ::CoTaskMemFree(id);
    return ok ? std::string(pfad) : std::string();
}

std::string DateiWaehlen(const char* titel, const char* filter,
                         const std::string& start) {
    char pfad[MAX_PATH] = {};
    OPENFILENAMEA of = {};
    of.lStructSize = sizeof(of);
    of.lpstrFilter = filter;
    of.lpstrFile = pfad;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = titel;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!start.empty()) of.lpstrInitialDir = start.c_str();
    return ::GetOpenFileNameA(&of) ? std::string(pfad) : std::string();
}

}  // namespace core
