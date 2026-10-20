#pragma once

namespace file_batch_cancel_cases
{
inline std::string Bytes(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

inline void Run(bool legacy = true, bool tracked = true)
{
    using namespace snowdesktop;
    using Mode = ::shell_worker_boundary::Cancellation;
    const std::array modes{Mode::LegacyAborted, Mode::LegacyCode, Mode::TrackedAborted,
        Mode::TrackedAbortResult, Mode::TrackedUserCancelled, Mode::TrackedSkipped};
    std::size_t scenes = 0;
    for (const auto mode : modes)
    for (const bool shellErrorUi : {false, true})
    {
        const bool tracksOutputs = mode >= Mode::TrackedAborted;
        if ((tracksOutputs && !tracked) || (!tracksOutputs && !legacy)) continue;
        const auto root = CreateTemporaryDirectory();
        struct Cleanup
        {
            std::filesystem::path root;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
        } cleanup{root};
        const auto target = root / L"target";
        std::filesystem::create_directory(target);
        const std::array files{root / L"A.txt", root / L"B.txt", root / L"C.txt",
            root / L"D.txt", root / L"E.txt", root / L"F.txt"};
        for (std::size_t i = 0; i < files.size(); ++i)
            std::ofstream(files[i], std::ios::binary) << "original-" << i;
        const auto check = [&](bool condition, const char* message) {
            if (!condition) throw std::runtime_error(std::string(message) + " [cancel mode=" +
                std::to_string(static_cast<int>(mode)) + " shellErrorUi=" + std::to_string(shellErrorUi) + "]");
        };
        ShellFileOperationRequest request;
        if (tracksOutputs) request.result = std::make_shared<ShellFileOperationResult>();
        const auto flags = static_cast<FILEOP_FLAGS>(shellErrorUi ? FOF_NOCONFIRMATION | FOF_SILENT : FOF_NO_UI);
        request.steps.push_back({FO_COPY, {files[0].wstring()}, target.wstring(), flags});
        request.steps.push_back({FO_COPY, {files[1].wstring(), files[3].wstring()}, target.wstring(), flags});
        request.steps.push_back({FO_COPY, {files[2].wstring()}, target.wstring(), flags});
        request.exactFileCopies.push_back({files[4].wstring(), (target / L"E.txt").wstring()});
        request.shortcuts.push_back({files[5].wstring(), (target / L"F.lnk").wstring(), root.wstring(), true});
        std::vector<operation_feedback::Failure> reports;
        operation_feedback::SetReporter([&](const auto& report) { reports.push_back(report); });
        ::shell_worker_boundary::Arm(mode, files[1].wstring());
        struct Reset
        {
            ~Reset() { operation_feedback::SetReporter({}); ::shell_worker_boundary::Disarm(); }
        } reset;
        ShellFileOperationWorker worker;
        std::promise<bool> complete;
        const auto callerThread = GetCurrentThreadId();
        std::atomic<DWORD> completionThread{callerThread};
        auto future = complete.get_future();
        check(worker.Enqueue(request, [&](bool succeeded) {
            completionThread = GetCurrentThreadId(); complete.set_value(succeeded);
        }), "D13: cancellation batch enters the real worker queue");
        const bool finished = future.wait_for(std::chrono::seconds(15)) == std::future_status::ready;
        check(finished, "D13: a cancelled real worker batch completes within its bound");
        const bool succeeded = future.get();
        worker.Stop();
        check(!succeeded && completionThread != callerThread && ::shell_worker_boundary::Calls() == 2,
            "D13: cancellation reports failure and stops before starting a later Shell step");
        check(std::distance(std::filesystem::directory_iterator(target), std::filesystem::directory_iterator{}) == 1 &&
            Bytes(target / L"A.txt") == "original-0",
            "D13: keep actually committed A and stop every later Shell, exact-copy and shortcut output");
        for (std::size_t i = 0; i < files.size(); ++i)
            check(Bytes(files[i]) == "original-" + std::to_string(i),
                "D13: cancellation preserves every original Copy source without rollback");
        if (tracksOutputs)
            check(request.result->outputs.size() == 1 && request.result->outputs[0].source == files[0].wstring() &&
                Bytes(request.result->outputs[0].destination) == "original-0",
                "D13: cancellation retains the successful real progress output for native placement");
        check(reports.size() == 1 && reports[0].messageKey == "app.operation.fileFailed" &&
            reports[0].warning && reports[0].error == ERROR_SUCCESS,
            "D13: cancelled partial work reports one existing localized incomplete-operation warning");
        check(reports[0].detail.find(files[0].wstring()) == std::wstring::npos,
            "D13: already completed A must not be listed among unfinished members");
        for (std::size_t i = 1; i < files.size(); ++i)
            check(reports[0].detail.find(files[i].wstring()) != std::wstring::npos,
                "D13: the cancelled step and every unstarted operation appear in actual feedback");
        ++scenes;
    }
    std::cout << scenes << " queued Shell cancellation and unfinished-feedback scenes passed\n";
}
}
