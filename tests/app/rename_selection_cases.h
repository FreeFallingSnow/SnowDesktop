#pragma once

void TestRenameSelectionUsesVisibleFileExtension()
{
    // Desktop, folder, Dock and quick-navigation editors share this selection
    // rule. Shell-hidden suffixes must not leave part of the basename behind
    // when the user types a replacement. Expectations are literal edit ranges.
    struct Case
    {
        const wchar_t* name;
        const wchar_t* path;
        bool directory;
        int selectionEnd;
        const char* message;
    };
    const Case cases[]{
        {L"Tool v1.2", L"C:\\Desktop\\Tool v1.2.lnk", false, -1,
            "a shortcut with hidden .lnk selects its entire dotted display name"},
        {L"Notes.txt", L"C:\\Desktop\\Notes.txt.lnk", false, -1,
            "a file-like shortcut name retains the visible .txt in the selection"},
        {L"Tool.lnk", L"C:\\Desktop\\Tool.lnk.lnk", false, -1,
            "a repeated shortcut suffix in the basename is still selected"},
        {L"Site.example", L"C:\\Desktop\\Site.example.url", false, -1,
            "an internet shortcut with hidden .url selects its full display name"},
        {L"Notes.v2", L"C:\\Desktop\\Notes.v2.txt", false, -1,
            "an ordinary file with hidden extension selects the complete basename"},
        {L"Notes.txt", L"C:\\Desktop\\Notes.txt", false, 5,
            "an ordinary visible extension remains outside the initial selection"},
        {L"Tool v1.2.lnk", L"C:\\Desktop\\Tool v1.2.lnk", false, 9,
            "a visible shortcut extension is excluded just like other visible suffixes"},
        {L"Archive.tar.gz", L"C:\\Desktop\\Archive.tar.gz", false, 11,
            "only the final visible extension is excluded for a multi-dot file"},
        {L"Notes.TXT", L"c:/desktop/notes.txt", false, 5,
            "Windows filename casing and forward-slash paths preserve extension selection"},
        {L"Folder.v2", L"C:\\Desktop\\Folder.v2", true, -1,
            "a dotted directory name is always selected in full"},
        {L"README", L"C:\\Desktop.v2\\README", false, -1,
            "dots in parent directories do not create a file extension"},
        {L".profile", L"C:\\Desktop\\.profile", false, -1,
            "a leading dot alone does not create an excluded extension"},
        {L"Notes.", L"C:\\Desktop\\Notes.", false, -1,
            "an empty suffix does not reduce the selection"},
        {L"Alias.txt", L"C:\\Desktop\\Other.txt", false, -1,
            "a Shell display alias is not mistaken for the actual filename"},
        {L"Notes.txt", L"", false, -1,
            "missing filesystem identity selects the full display name"},
        {L"", L"", false, -1,
            "an empty display name keeps select-all semantics"},
    };
    for (const auto& test : cases)
        Check(RenameInitialSelectionEnd(test.name, test.path, test.directory) ==
            test.selectionEnd, test.message);
}
