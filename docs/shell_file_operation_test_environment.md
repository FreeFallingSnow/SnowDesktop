# Shell 文件操作测试的环境边界

`scripts/test.bat name shell_file_operation_worker` 以及完整测试中的同名条目，验证后台队列和 Windows Shell 的真实复制、移动、重命名、快捷方式、拖放交付、通知及部分失败行为。

默认模式在首次 COM/Shell 调用前，为**当前测试进程**启用 `ProcessSignaturePolicy.MicrosoftSignedOnly`。该 Windows 策略限制后续非 Microsoft 签名 DLL 的加载；不修改系统注册表、Explorer、SnowDesktop 主程序或用户安装的扩展。设置策略失败会使测试失败，不静默退回未经隔离的环境。[Microsoft 文档](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-process_mitigation_binary_signature_policy)

所有原有业务断言、真实文件操作及正常退出检查均保留。没有用 mock 替代 Shell，没有延长超时，也没有在输出成功后强制退出。默认模式不能作为已安装第三方 Shell 扩展的兼容性证据。

## 第三方扩展诊断

需要保留本机扩展环境时，先通过标准测试入口生成目标，再运行：

```powershell
& .build/Release/tests/SnowDesktopShellFileOperationWorkerTests.exe --with-third-party-shell-extensions
```

该模式显式记录扩展环境，不属于默认全量回归的环境。外部运行器应保留有界超时与失败日志；输出成功不等于进程正常退出。若此模式超时，不得将默认模式通过解释为扩展兼容性已修复。

## 2026-09-14 调查依据与未关闭事项

- 原始全量结果为 117/118；本条目打印成功后被 CTest 判为 Timeout（150.68 秒）。之后同程序也有正常退出样本，属于间歇性问题。
- 调试样本进入 `ntdll!RtlExitUserProcess` 后仍未正常完成观察窗口。成功输出位于显式停止工作线程、清理临时文件和 `CoUninitialize()` 之后；尚未取得足以锁定精确阻塞函数的完整退出栈。
- 隔离副本确认本机加载了 WPS Office 12.1.0.28505 的 `qingnse64.dll` 和 `kbaseconfigcenter64.dll`；`CoFreeUnusedLibrariesEx(0, 0)` 后仍驻留。启用上述加载策略的副本不再加载这两个 DLL，同一组真实 Shell 断言正常完成。
- 这证明进程级隔离有效，但尚不足以将原始超时唯一归因于 WPS。带第三方扩展的环境及应用退出兼容性仍待独立验证，不因受控回归通过而关闭。
- 原始全量 JUnit：`.build/Testing/test-run-a621473b931444c5a352e7bb5ccb4c34.xml`。本地调查与对照记录位于 `.codex-probes/shell-exit-investigation/` 和 `.codex-probes/shell-exit-fix/`。
