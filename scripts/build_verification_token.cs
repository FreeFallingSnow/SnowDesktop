using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Principal;

namespace SnowDesktop.Verification {
 public static class NativeToken {
  [DllImport("advapi32.dll",SetLastError=true)] static extern bool OpenProcessToken(IntPtr process,uint access,out IntPtr token);
  [DllImport("advapi32.dll",SetLastError=true)] static extern bool OpenThreadToken(IntPtr thread,uint access,bool asSelf,out IntPtr token);
  [DllImport("advapi32.dll",SetLastError=true)] static extern bool GetTokenInformation(IntPtr token,int kind,IntPtr data,int length,out int returned);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentThread();
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  static IntPtr Info(IntPtr token,int kind,out int length) {
   GetTokenInformation(token,kind,IntPtr.Zero,0,out length);
   if(length<=0||length>1024*1024)throw new InvalidOperationException("Native token information size is unavailable.");
   IntPtr data=Marshal.AllocHGlobal(length);
   if(!GetTokenInformation(token,kind,data,length,out length)){int error=Marshal.GetLastWin32Error();Marshal.FreeHGlobal(data);throw new Win32Exception(error);}
   return data;
  }
  static string Sid(IntPtr token,int kind) {
   int length;IntPtr data=Info(token,kind,out length);
   try {if(length<IntPtr.Size)throw new InvalidOperationException("Native token SID is truncated.");IntPtr sid=Marshal.ReadIntPtr(data);return sid==IntPtr.Zero?"none":new SecurityIdentifier(sid).Value;}
   finally {Marshal.FreeHGlobal(data);}
  }
  static string Scalar(IntPtr token,int kind) {
   int length;IntPtr data=Info(token,kind,out length);
   try {if(length<4)throw new InvalidOperationException("Native token field is truncated.");return unchecked((uint)Marshal.ReadInt32(data)).ToString("x8",CultureInfo.InvariantCulture);}
   finally {Marshal.FreeHGlobal(data);}
  }
  static long Statistic(IntPtr token,int offset) {
   int length;IntPtr data=Info(token,10,out length);
   try {if(length<offset+8)throw new InvalidOperationException("Native token statistics are truncated.");return Marshal.ReadInt64(data,offset);}
   finally {Marshal.FreeHGlobal(data);}
  }
  static void Groups(IntPtr token,int kind,string label,List<string> parts) {
   int length;IntPtr data=Info(token,kind,out length);
   try {
    if(length<4)throw new InvalidOperationException("Native token group header is truncated.");
    int count=Marshal.ReadInt32(data),offset=IntPtr.Size,stride=IntPtr.Size*2;
    if(count<0||count>16384||(count>0&&length<offset+count*stride))throw new InvalidOperationException("Native token groups exceed their buffer.");
    for(int i=0;i<count;++i){int item=offset+i*stride;string sid=new SecurityIdentifier(Marshal.ReadIntPtr(data,item)).Value;uint attributes=unchecked((uint)Marshal.ReadInt32(data,item+IntPtr.Size));parts.Add(label+":"+sid+":"+attributes.ToString("x8",CultureInfo.InvariantCulture));}
   } finally {Marshal.FreeHGlobal(data);}
  }
  static void Privileges(IntPtr token,List<string> parts) {
   int length;IntPtr data=Info(token,3,out length);
   try {
    if(length<4)throw new InvalidOperationException("Native token privilege header is truncated.");int count=Marshal.ReadInt32(data);
    if(count<0||count>512||length<4+count*12)throw new InvalidOperationException("Native token privileges exceed their buffer.");
    for(int i=0;i<count;++i){int item=4+i*12;long luid=Marshal.ReadInt64(data,item);uint attributes=unchecked((uint)Marshal.ReadInt32(data,item+8));parts.Add("privilege:"+luid.ToString("x16",CultureInfo.InvariantCulture)+":"+attributes.ToString("x8",CultureInfo.InvariantCulture));}
   } finally {Marshal.FreeHGlobal(data);}
  }
  public static string Capture() {
   IntPtr token;
   if(!OpenThreadToken(GetCurrentThread(),8,true,out token)){
    int error=Marshal.GetLastWin32Error();if(error!=1008)throw new Win32Exception(error);
    if(!OpenProcessToken(GetCurrentProcess(),8,out token))throw new Win32Exception(Marshal.GetLastWin32Error());
   }
   try {
    for(int attempt=0;attempt<3;++attempt){
     long modified=Statistic(token,48);var parts=new List<string>();
     parts.Add("windows-token-v1");parts.Add("authentication:"+Statistic(token,8).ToString("x16",CultureInfo.InvariantCulture));
     parts.Add("user:"+Sid(token,1));parts.Add("owner:"+Sid(token,4));parts.Add("primary-group:"+Sid(token,5));parts.Add("integrity:"+Sid(token,25));
     foreach(int kind in new int[]{12,18,20,23,24,26,27,29})parts.Add("field:"+kind.ToString(CultureInfo.InvariantCulture)+":"+Scalar(token,kind));
     Groups(token,2,"group",parts);Groups(token,11,"restricted",parts);Groups(token,30,"capability",parts);Privileges(token,parts);
     if(Statistic(token,48)!=modified)continue;
     // Token/modified IDs differ between processes and are not permissions.
     // Authentication ID, actual attributes and enabled rights remain bound.
     parts.Sort(StringComparer.Ordinal);return String.Join("\n",parts.ToArray());
    }
    throw new InvalidOperationException("Native token changed while being observed.");
   } finally {CloseHandle(token);}
  }
 }
}
