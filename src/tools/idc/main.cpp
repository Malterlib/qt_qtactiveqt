/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the tools applications of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QDir>
#include <QFile>
#include <QDir>
#include <QScopedArrayPointer>
#include <qt_windows.h>
#include <io.h>

QT_BEGIN_NAMESPACE

static QString quotePath(const QString &s)
{
    if (!s.startsWith(QLatin1Char('\"')) && s.contains(QLatin1Char(' ')))
        return QLatin1Char('\"') + s + QLatin1Char('\"');
    return s;
}

static bool hasFileExtension(const QString &filePath, const QString &extension)
{
    return filePath.endsWith(extension, Qt::CaseInsensitive);
}

static bool hasExeExtension(const QString &filePath)
{
    return hasFileExtension(filePath, QStringLiteral(".exe"));
}

static bool hasDllExtension(const QString &filePath)
{
    return hasFileExtension(filePath, QStringLiteral(".dll"));
}

// Prepend the Qt binary directory to PATH.
static bool prependPath()
{
    enum { maxEnvironmentSize = 32767 };
    wchar_t buffer[maxEnvironmentSize];
    if (!GetModuleFileName(NULL, buffer, maxEnvironmentSize))
        return false;
    wchar_t *ptr = wcsrchr(buffer, L'\\');
    if (!ptr)
        return false;
    *ptr++ = L';';
    const wchar_t pathVariable[] = L"PATH";
    if (!GetEnvironmentVariable(pathVariable, ptr, maxEnvironmentSize - (ptr - buffer))
        || !SetEnvironmentVariable(pathVariable, buffer)) {
        return false;
    }
    return true;
}

static QString errorString(DWORD errorCode)
{
    wchar_t *resultW = 0;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM,
                  NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPWSTR)&resultW, 0, NULL);
    const QString result = QString::fromWCharArray(resultW);
    LocalFree((HLOCAL)resultW);
    return result;
}

static bool runWithQtInEnvironment(const QString &cmd)
{
    enum { timeOutMs = 30000 };
    static const bool pathSet = prependPath();
    if (!pathSet)
        return false;

    STARTUPINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    STARTUPINFO myInfo;
    GetStartupInfo(&myInfo);
    si.hStdInput = myInfo.hStdInput;
    si.hStdOutput = myInfo.hStdOutput;
    si.hStdError = myInfo.hStdError;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

    QScopedArrayPointer<wchar_t> commandLineW(new wchar_t[cmd.size() + 1]);
    cmd.toWCharArray(commandLineW.data());
    commandLineW[cmd.size()] = 0;
    if (!CreateProcessW(0, commandLineW.data(), 0, 0, /* InheritHandles */ TRUE, 0, 0, 0, &si, &pi)) {
        fprintf(stderr, "Unable to execute \"%s\": %s\n", qPrintable(cmd),
                qPrintable(errorString(GetLastError())));
        return false;
    }

    DWORD exitCode = 1;
    switch (WaitForSingleObject(pi.hProcess, timeOutMs)) {
    case WAIT_OBJECT_0:
        GetExitCodeProcess(pi.hProcess, &exitCode);
        break;
    case WAIT_TIMEOUT:
        fprintf(stderr, "Timed out after %d ms out waiting for \"%s\".\n",
                int(timeOutMs), qPrintable(cmd));
        TerminateProcess(pi.hProcess, 1);
        break;
    default:
        fprintf(stderr, "Error waiting for \"%s\": %s\n",
                qPrintable(cmd), qPrintable(errorString(GetLastError())));
        TerminateProcess(pi.hProcess, 1);
        break;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode)
        fprintf(stderr, "\"%s\" returned exit code: %lu (0x%lx)\n", qPrintable(cmd), exitCode, exitCode);
    return exitCode == 0;
}

static bool attachTypeLibrary(const QString &applicationName, int resource, const QByteArray &data, QString *errorMessage)
{
    HANDLE hExe = BeginUpdateResource((const wchar_t *)applicationName.utf16(), false);
    if (hExe == 0) {
        if (errorMessage)
            *errorMessage = QString::fromLatin1("Failed to attach type library to binary %1 - could not open file.").arg(applicationName);
        return false;
    }
    if (!UpdateResource(hExe, L"TYPELIB", MAKEINTRESOURCE(resource), 0, (void*)data.data(), data.count())) {
        EndUpdateResource(hExe, true);
        if (errorMessage)
            *errorMessage = QString::fromLatin1("Failed to attach type library to binary %1 - could not update file.").arg(applicationName);
        return false;
    }

    if (!EndUpdateResource(hExe,false)) {
        if (errorMessage)
            *errorMessage = QString::fromLatin1("Failed to attach type library to binary %1 - could not write file.").arg(applicationName);
        return false;
    }

    if (errorMessage)
        *errorMessage = QString::fromLatin1("Type library attached to %1.").arg(applicationName);
    return true;
}

// Manually add defines that are missing from pre-VS2012 compilers
#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#  define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#  define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif

static HMODULE loadLibraryQt(const QString &input)
{
    if (QSysInfo::windowsVersion() < QSysInfo::WV_VISTA)
        return LoadLibrary((const wchar_t *)input.utf16()); // fallback for Windows XP and older

    // Load DLL with the folder containing the DLL temporarily added to the search path when loading dependencies
    return LoadLibraryEx((const wchar_t *)input.utf16(), NULL,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

static bool registerServer(const QString &input)
{
    bool ok = false;
    if (hasExeExtension(input)) {
        ok = runWithQtInEnvironment(quotePath(input) + QLatin1String(" -regserver"));
    } else {
        HMODULE hdll = loadLibraryQt(input);
        if (!hdll) {
            fprintf(stderr, "Couldn't load library file %s\n", qPrintable(input));
            return false;
        }
        typedef HRESULT(__stdcall* RegServerProc)();
        RegServerProc DllRegisterServer = (RegServerProc)GetProcAddress(hdll, "DllRegisterServer");
        if (!DllRegisterServer) {
            fprintf(stderr, "Library file %s doesn't appear to be a COM library\n", qPrintable(input));
            return false;
        }
        ok = DllRegisterServer() == S_OK;
    }
    return ok;
}

static bool unregisterServer(const QString &input)
{
    bool ok = false;
    if (hasExeExtension(input)) {
        ok = runWithQtInEnvironment(quotePath(input) + QLatin1String(" -unregserver"));
    } else {
        HMODULE hdll = loadLibraryQt(input);
        if (!hdll) {
            fprintf(stderr, "Couldn't load library file %s\n", qPrintable(input));
            return false;
        }
        typedef HRESULT(__stdcall* RegServerProc)();
        RegServerProc DllUnregisterServer = (RegServerProc)GetProcAddress(hdll, "DllUnregisterServer");
        if (!DllUnregisterServer) {
            fprintf(stderr, "Library file %s doesn't appear to be a COM library\n", qPrintable(input));
            return false;
        }
        ok = DllUnregisterServer() == S_OK;
    }
    return ok;
}

static HRESULT dumpIdl(const QString &input, const QString &idlfile, const QString &version)
{
    HRESULT res = E_FAIL;

    if (hasExeExtension(input)) {
        if (runWithQtInEnvironment(quotePath(input) + QLatin1String(" -dumpidl ") + idlfile + QLatin1String(" -version ") + version))
            res = S_OK;
    } else {
        HMODULE hdll = loadLibraryQt(input);
        if (!hdll) {
            fprintf(stderr, "Couldn't load library file %s\n", qPrintable(input));
            return 3;
        }
        typedef HRESULT(__stdcall* DumpIDLProc)(const QString&, const QString&);
        DumpIDLProc DumpIDL = (DumpIDLProc)GetProcAddress(hdll, "DumpIDL");
        if (!DumpIDL) {
            fprintf(stderr, "Couldn't resolve 'DumpIDL' symbol in %s\n", qPrintable(input));
            return 3;
        }
        res = DumpIDL(idlfile, version);
        FreeLibrary(hdll);
    }

    return res;
}

int runIdc(int argc, char **argv)
{
    QString error;
    QString tlbfile;
    QString idlfile;
    QString input;
    QString version = QLatin1String("1.0");

    int i = 1;
    while (i < argc) {
        QString p = QString::fromLocal8Bit(argv[i]).toLower();

        if (p == QLatin1String("/idl") || p == QLatin1String("-idl")) {
            ++i;
            if (i > argc) {
                error = QLatin1String("Missing name for interface definition file!");
                break;
            }
            idlfile = QFile::decodeName(argv[i]).trimmed();
        } else if (p == QLatin1String("/version") || p == QLatin1String("-version")) {
            ++i;
            if (i > argc)
                version = QLatin1String("1.0");
            else
                version = QLatin1String(argv[i]);
        } else if (p == QLatin1String("/tlb") || p == QLatin1String("-tlb")) {
            ++i;
            if (i > argc) {
                error = QLatin1String("Missing name for type library file!");
                break;
            }
            tlbfile = QFile::decodeName(argv[i]).trimmed();
        } else if (p == QLatin1String("/v") || p == QLatin1String("-v")) {
            fprintf(stdout, "Qt Interface Definition Compiler version 1.0\n");
            return 0;
        } else if (p == QLatin1String("/regserver") || p == QLatin1String("-regserver")) {
            if (!registerServer(input)) {
                fprintf(stderr, "Failed to register server!\n");
                return 1;
            }
            fprintf(stderr, "Server registered successfully!\n");
            return 0;
        } else if (p == QLatin1String("/unregserver") || p == QLatin1String("-unregserver")) {
            if (!unregisterServer(input)) {
                fprintf(stderr, "Failed to unregister server!\n");
                return 1;
            }
            fprintf(stderr, "Server unregistered successfully!\n");
            return 0;
        } else if (p[0] == QLatin1Char('/') || p[0] == QLatin1Char('-')) {
            error = QLatin1String("Unknown option \"") + p + QLatin1Char('"');
            break;
        } else {
            input = QFile::decodeName(argv[i]).trimmed();
            // LoadLibraryEx requires a fully qualified path when used together with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
            input = QFileInfo(input).absoluteFilePath();
            input = QDir::toNativeSeparators(input);
        }
        i++;
    }
    if (!error.isEmpty()) {
        fprintf(stderr, "%s\n", qPrintable(error));
        return 5;
    }
    if (input.isEmpty()) {
        fprintf(stderr, "No input file specified!\n");
        return 1;
    }
    if (hasExeExtension(input) && tlbfile.isEmpty() && idlfile.isEmpty()) {
        fprintf(stderr, "No type output file specified!\n");
        return 2;
    }
    if (hasDllExtension(input) && idlfile.isEmpty() && tlbfile.isEmpty()) {
        fprintf(stderr, "No interface definition file and no type library file specified!\n");
        return 3;
    }
    if (!tlbfile.isEmpty()) {
        tlbfile = QDir::toNativeSeparators(tlbfile);
        QFile file(tlbfile);
        if (!file.open(QIODevice::ReadOnly)) {
            fprintf(stderr, "Couldn't open %s for read: %s\n", qPrintable(tlbfile), qPrintable(file.errorString()));
            return 4;
        }
        QString error;
        const bool ok = attachTypeLibrary(input, 1, file.readAll(), &error);
        fprintf(stderr, "%s\n", qPrintable(error));
        return ok ? 0 : 4;
    } else if (!idlfile.isEmpty()) {
        idlfile = quotePath(QDir::toNativeSeparators(idlfile));
        fprintf(stderr, "\n\n%s\n\n", qPrintable(idlfile));
        const HRESULT res = dumpIdl(input, idlfile, version);

        switch (res) {
        case S_OK:
            break;
        case E_FAIL:
            fprintf(stderr, "IDL generation failed trying to run program %s!\n", qPrintable(input));
            return res;
        case -1:
            fprintf(stderr, "Couldn't open %s for writing!\n", qPrintable(idlfile));
            return res;
        case 1:
            fprintf(stderr, "Malformed appID value in %s!\n", qPrintable(input));
            return res;
        case 2:
            fprintf(stderr, "Malformed typeLibID value in %s!\n", qPrintable(input));
            return res;
        case 3:
            fprintf(stderr, "Class has no metaobject information (error in %s)!\n", qPrintable(input));
            return res;
        case 4:
            fprintf(stderr, "Malformed classID value in %s!\n", qPrintable(input));
            return res;
        case 5:
            fprintf(stderr, "Malformed interfaceID value in %s!\n", qPrintable(input));
            return res;
        case 6:
            fprintf(stderr, "Malformed eventsID value in %s!\n", qPrintable(input));
            return res;

        default:
            fprintf(stderr, "Unknown error writing IDL from %s\n", qPrintable(input));
            return 7;
        }
    }
    return 0;
}

QT_END_NAMESPACE

int main(int argc, char **argv)
{
    return QT_PREPEND_NAMESPACE(runIdc)(argc, argv);
}
