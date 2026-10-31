    static void CheckWorkshopUnsubscribeCompletion(DesktopApp& app)
    {
        using Result = snowdesktop::winui::WidgetsPageHostOperationResult;
        const auto require = [](bool value, const char* message) {
            if (!value) throw std::runtime_error(message);
        };
        app.clipboardReadWork_.Stop();
        app.dataCleanupWorker_.Stop();
        app.iconWork_.Stop();
        app.widgetEngine_ = std::make_unique<WidgetEngine>();
        // No Steam request is started: supply the real host's completed query
        // and keep its existing in-flight guard set while callbacks are checked.
        const auto poll = [&](bool available, bool subscribed, bool unrelatedError, bool delayed = false) {
            app.steamWorkshopSubscriptionPollState_ =
                std::make_shared<SteamWorkshopSubscriptionPollState>();
            auto& state = *app.steamWorkshopSubscriptionPollState_;
            state.queryInFlight.store(true);
            state.nextQueryId = 18;
            SteamWorkshopSubscriptionPollState::ReadyQuery ready;
            ready.queryId = 17;
            ready.snapshot.authoritative = available;
            if (subscribed)
                ready.snapshot.subscribedPublishedFileIds = {"3829336395@111"};
            if (!available) ready.snapshot.error = "isolated subscription query unavailable";
            if (unrelatedError)
            {
                snowdesktop::widget::SteamWorkshopInstallFailure failure;
                failure.packageId = "39f065d6-4cef-4e7a-a682-e9693da3c51c";
                failure.error = "update requests a new optional permission: clipboard.write";
                ready.snapshot.discoveryFailures.push_back(std::move(failure));
            }
            state.ready = std::move(ready);
            std::optional<Result> unsubscribeResult;
            std::optional<Result> syncResult;
            int unsubscribeCalls = 0;
            int syncCalls = 0;
            SteamWorkshopSubscriptionPollState::SettingsCompletion unsubscribe;
            unsubscribe.queryId = 17;
            unsubscribe.expectedUnsubscribedPublishedFileId = "3829336395";
            unsubscribe.expectedRemovedPackageIds = {"4c360510-dd3d-4d3d-9496-686477fcfa37"};
            if (!delayed)
                unsubscribe.reconciliationDeadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            unsubscribe.done = [&](Result result) { ++unsubscribeCalls; unsubscribeResult = std::move(result); };
            state.settingsCompletions.push_back(std::move(unsubscribe));
            SteamWorkshopSubscriptionPollState::SettingsCompletion sync;
            sync.queryId = 17;
            sync.done = [&](Result result) { ++syncCalls; syncResult = std::move(result); };
            state.settingsCompletions.push_back(std::move(sync));
            app.PollSteamWorkshopSubscriptions(false);
            if (delayed)
            {
                require(!unsubscribeResult && syncResult && unsubscribeCalls == 0 && syncCalls == 1 &&
                    state.settingsCompletions.size() == 1 && state.refreshPending.load(),
                    "Steam cache lag retains only the unsubscribe while full sync reports its actual result");
                const auto deadline = state.settingsCompletions.front().reconciliationDeadline;
                require(deadline > std::chrono::steady_clock::now(),
                    "a pending operation has a finite monotonic reconciliation deadline");
                SteamWorkshopSubscriptionPollState::ReadyQuery fresh;
                fresh.queryId = 18;
                fresh.snapshot.authoritative = true;
                state.ready = std::move(fresh);
                state.nextQueryId = 19;
                app.PollSteamWorkshopSubscriptions(false);
            }
            require(unsubscribeResult && syncResult && state.settingsCompletions.empty(),
                "the actual host dispatches both operation completions exactly once");
            require(unsubscribeCalls == 1 && syncCalls == 1,
                "fresh cache confirmation completes the retained callback without redelivering full sync");
            require(unsubscribeResult->succeeded == (delayed || (available && !subscribed)),
                "a reconciled target unsubscribe succeeds despite an unrelated permission update failure");
            require(syncResult->succeeded == (available && !unrelatedError),
                "a full synchronization still reports its unrelated update failure");
            if (delayed || (available && !subscribed))
            {
                require(unsubscribeResult->message.empty(),
                    "unsubscribe success selects the page's own success message, without foreign errors");
            }
            if (available && unrelatedError)
                require(syncResult->message.find(L"clipboard.write") != std::wstring::npos,
                    "the original permission failure remains visible to the full-sync caller");
        };
        poll(true, false, true);
        poll(true, true, true);
        poll(false, false, false);
        poll(true, false, false);
        poll(true, true, true, true);
        poll(false, false, false, true);
    }
