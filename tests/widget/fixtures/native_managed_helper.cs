// Data fixture for the C++ native helper tests. Compile with the installed
// .NET Framework v4 csc, then set SNOWDESKTOP_NATIVE_TEST_HELPER to its EXE.
using System;
using System.IO;
using System.Text;

class NativeManagedHelper
{
    static readonly Stream Output = Console.OpenStandardOutput();
    static void Write(string line, Encoding encoding)
    {
        byte[] bytes = encoding.GetBytes(line + "\n");
        Output.Write(bytes, 0, bytes.Length);
        Output.Flush();
    }
    static int Main(string[] args)
    {
        var utf8 = new UTF8Encoding(false, true);
        var input = new StreamReader(Console.OpenStandardInput(), utf8, false);
        string hello = input.ReadLine();
        if (hello == null || !hello.Contains("snowdesktop-native")) return 2;
        Write("{\"version\":1,\"kind\":\"hello\",\"protocol\":\"snowdesktop-native\"}", utf8);
        string request = input.ReadLine();
        if (request == null) return 3;
        string arg = args.Length == 1 ? args[0] : "";
        arg = arg.Replace("\\", "\\\\").Replace("\"", "\\\"");
        string result = "{\"version\":1,\"kind\":\"result\",\"id\":\"1\",\"value\":{\"argc\":" +
            args.Length + ",\"arg\":\"" + arg + "\",\"label\":\"\u9f20\u6807 \u00b7 2.4G\"}}";
        Write(result, request.Contains("\"operation\":\"gbk\"") ? Encoding.GetEncoding(936) : utf8);
        input.ReadLine();
        return 0;
    }
}
