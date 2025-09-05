#include "StdAfx.h"
#include <vector>
#include <string>
#include "cpp/Common/MyWindows.h"
#include "cpp/Common/MyInitGuid.h"
#include "cpp/Common/Defs.h"
#include "cpp/Common/StringConvert.h"
#include "cpp/Windows/DLL.h"
#include "cpp/Windows/FileDir.h"
#include "cpp/Windows/FileFind.h"
#include "cpp/Windows/PropVariant.h"
#include "cpp/Windows/PropVariantConv.h"
#include "cpp/Windows/TimeUtils.h"
#include "cpp/7zip/Common/FileStreams.h"
#include "cpp/7zip/Archive/IArchive.h"
#include "cpp/7zip/IPassword.h"

#include <Shlwapi.h>
#include <iostream>
#include <fstream>
#pragma comment(lib, "Shlwapi.lib")

#ifdef _WIN32
extern HINSTANCE g_hInstance;
HINSTANCE g_hInstance = NULL;
#endif

Z7_DIAGNOSTIC_IGNORE_CAST_FUNCTION

#define DEFINE_GUID_ARC(name, id) Z7_DEFINE_GUID(name, \
  0x23170F69, 0x40C1, 0x278A, 0x10, 0x00, 0x00, 0x01, 0x10, id, 0x00, 0x00);

enum { kId_7z = 7 };
DEFINE_GUID_ARC(CLSID_Format, kId_7z)

using namespace NWindows;
using namespace NFile;
using namespace NDir;
using namespace NWindows::NCOM;

#ifdef _WIN32
#define kDllName "7z.dll"
#else
#define kDllName "7z.so"
#endif

struct CDirItem: public NWindows::NFile::NFind::CFileInfoBase {
  UString path_for_handler;
  FString full_path;
  
  CDirItem(const NWindows::NFile::NFind::CFileInfo &fi):
      CFileInfoBase(fi) {}
};

class CArchiveUpdateCallback Z7_final:
  public IArchiveUpdateCallback2,
  public ICryptoGetTextPassword2,
  public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_2(IArchiveUpdateCallback2, ICryptoGetTextPassword2)
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IArchiveUpdateCallback)

public:
  const CObjectVector<CDirItem> *dir_items_;
  bool password_is_defined_;
  UString password_;
  
  CArchiveUpdateCallback(): dir_items_(NULL), password_is_defined_(false) {}
  
  void Init(const CObjectVector<CDirItem> *dir_items, const UString &password) {
    dir_items_ = dir_items;
    password_ = password;
    password_is_defined_ = !password.IsEmpty();
  }
};

Z7_COM7F_IMF(CArchiveUpdateCallback::SetTotal(UInt64 size)) {
  std::cout << "Total size: " << size << std::endl;
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::SetCompleted(const UInt64 *completeValue)) {
  std::cout << "Completed: " << *completeValue << std::endl;
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetUpdateItemInfo(UInt32 index,
      Int32 *new_data, Int32 *new_properties, UInt32 *index_in_archive)) {
  if (new_data) *new_data = BoolToInt(true);
  if (new_properties) *new_properties = BoolToInt(true);
  if (index_in_archive) *index_in_archive = (UInt32)(Int32)-1;
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetProperty(UInt32 index, PROPID prop_id, PROPVARIANT *value)) {
  if (index >= dir_items_->Size()) {
    return E_INVALIDARG;
  }
  const CDirItem &dir_item = (*dir_items_)[index];
  
  CPropVariant prop;  // 使用CPropVariant而不是直接操作PROPVARIANT
  switch (prop_id) {
    case kpidPath: prop = dir_item.path_for_handler; break;
    case kpidIsDir: prop = dir_item.IsDir(); break;
    case kpidSize: if (!dir_item.IsDir()) prop = dir_item.Size; break;
    case kpidMTime: PropVariant_SetFrom_FiTime(prop, dir_item.MTime); break;
    case kpidCTime: PropVariant_SetFrom_FiTime(prop, dir_item.CTime); break;  // 添加创建时间支持
    case kpidATime: PropVariant_SetFrom_FiTime(prop, dir_item.ATime); break;  // 可选：添加访问时间支持
  }
  prop.Detach(value);  // 使用Detach方法
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetStream(UInt32 index, ISequentialInStream **in_stream)) {
  const CDirItem &dir_item = (*dir_items_)[index];
  if (dir_item.IsDir()) return S_OK;
  CInFileStream *in_file_stream_spec = new CInFileStream;
  CMyComPtr<ISequentialInStream> in_file_stream(in_file_stream_spec);
  if (!in_file_stream_spec->Open(dir_item.full_path)) return GetLastError_noZero_HRESULT();
  *in_stream = in_file_stream.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::SetOperationResult(Int32 operationResult)) {
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetVolumeSize(UInt32 /* index */, UInt64 * /* size */)) {
  return S_FALSE;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetVolumeStream(UInt32 /* index */, ISequentialOutStream ** /* volumeStream */)) {
  return S_FALSE;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::CryptoGetTextPassword2(Int32 *password_is_defined, BSTR *password)) {
  *password_is_defined = BoolToInt(password_is_defined_);
  return StringToBstr(password_, password);
}

class FileCompressor {
public:
  // 创建普通7z压缩文件
  static bool CompressFiles(const std::vector<std::wstring>& file_paths, 
                           const std::wstring& archive_path,
                           const std::wstring& password = L"") {
    return CreateArchive(file_paths, archive_path, password, false);
  }
  
  // 创建自解压文件
  static bool CreateSelfExtractingArchive(const std::vector<std::wstring>& file_paths,
                                         const std::wstring& sfx_path,
                                         const std::wstring& password = L"",
                                         const std::wstring& sfx_module_path = L"") {
    // 首先创建临时的7z文件
    std::wstring temp_7z_path = sfx_path + L".tmp.7z";
    
    if (!CreateArchive(file_paths, temp_7z_path, password, true)) {
      return false;
    }
    
    // 确定SFX模块路径
    std::wstring sfx_module = sfx_module_path;
    if (sfx_module.empty()) {
      wchar_t module_path[MAX_PATH];
      GetModuleFileName(NULL, module_path, MAX_PATH);
      PathRemoveFileSpec(module_path);
      // 默认使用7z.sfx（GUI版本）
      PathAppend(module_path, L"7z.sfx");
      sfx_module = module_path;
      if ( !::PathFileExists(sfx_module.c_str())) {
        return false;
      }
    }
    
    // 创建自解压文件：SFX模块 + 7z数据
    bool success = CombineSfxAndArchive(sfx_module, temp_7z_path, sfx_path);
    
    // 清理临时文件
    ::DeleteFile(temp_7z_path.c_str());
    
    return success;
  }

private:
  // 创建7z压缩文件的核心函数
  static bool CreateArchive(const std::vector<std::wstring>& file_paths,
                           const std::wstring& archive_path,
                           const std::wstring& password,
                           bool for_sfx) {
    // Load 7z library
    FString dll_prefix = NDLL::GetModuleDirPrefix();
    NDLL::CLibrary lib;
    if (!lib.Load(dll_prefix + FTEXT(kDllName))) return false;
    
    Func_CreateObject f_create_object = Z7_GET_PROC_ADDRESS(
        Func_CreateObject, lib.Get_HMODULE(), "CreateObject");
    if (!f_create_object) return false;
    
    // Collect files
    CObjectVector<CDirItem> dir_items;
    for (const auto& file_path : file_paths) {
      FString fs_path = us2fs(UString(file_path.c_str()));
      NFind::CFileInfo file_info;
      if (!file_info.Find(fs_path)) return false;
      
      CDirItem dir_item(file_info);
      dir_item.path_for_handler = fs2us(fs_path.Ptr(fs_path.ReverseFind_PathSepar() + 1));
      dir_item.full_path = fs_path;
      dir_items.Add(dir_item);
    }
    
    if (dir_items.Size() == 0) return false;
    
    // Create output file
    FString archive_name = us2fs(UString(archive_path.c_str()));
    COutFileStream* out_file_stream_spec = new COutFileStream;
    CMyComPtr<IOutStream> out_file_stream = out_file_stream_spec;
    if (!out_file_stream_spec->Create_NEW(archive_name)) return false;
    
    // Create archive object
    CMyComPtr<IOutArchive> out_archive;
    if (f_create_object(&CLSID_Format, &IID_IOutArchive, 
                       (void**)&out_archive) != S_OK) return false;
    
    // Set compression properties
    CMyComPtr<ISetProperties> set_properties;
    if (out_archive->QueryInterface(IID_ISetProperties, (void**)&set_properties) == S_OK) {
      const wchar_t *names[] = {L"x", L"tc"};
      PROPVARIANT values_xx[2] = {};
      
      // 压缩等级
      values_xx[0].vt = VT_UI4;
      values_xx[0].ulVal = for_sfx ? 7 : 9;  // SFX使用稍低压缩等级以提高速度
      
      // 时间戳选项
      values_xx[1].vt = VT_BOOL;
      values_xx[1].boolVal = VARIANT_TRUE;  // 创建时间
      
      auto set_ret = set_properties->SetProperties(names, values_xx, 2);
    }
    
    // Create update callback
    CArchiveUpdateCallback* update_callback_spec = new CArchiveUpdateCallback;
    CMyComPtr<IArchiveUpdateCallback2> update_callback(update_callback_spec);
    update_callback_spec->Init(&dir_items, UString(password.c_str()));
    
    // Perform compression
    HRESULT result = out_archive->UpdateItems(out_file_stream, 
                                             dir_items.Size(), 
                                             update_callback);
    return result == S_OK;
  }
  
  // 合并SFX模块和7z数据创建自解压文件
  static bool CombineSfxAndArchive(const std::wstring& sfx_module_path,
                                  const std::wstring& archive_path,
                                  const std::wstring& output_path) {
    std::ifstream sfx_file(sfx_module_path, std::ios::binary);
    std::ifstream archive_file(archive_path, std::ios::binary);
    std::ofstream output_file(output_path, std::ios::binary);
    
    if (!sfx_file.is_open() || !archive_file.is_open() || !output_file.is_open()) {
      return false;
    }
    
    // 复制SFX模块
    output_file << sfx_file.rdbuf();
    
    // 复制7z数据
    output_file << archive_file.rdbuf();
    
    sfx_file.close();
    archive_file.close();
    output_file.close();
    
    std::wcout << L"Self-extracting archive created: " << output_path << std::endl;
    return true;
  }
};

int main() {
  // 示例：创建自解压文件
  std::vector<std::wstring> files = {
      LR"(C:\Users\ewing\Desktop\zip_test\7zFM.exe)",
      LR"(C:\Users\ewing\Desktop\zip_test\7zG.exe)",
      LR"(C:\Users\ewing\Desktop\zip_test\7-zip.chm)",
      LR"(C:\Users\ewing\Desktop\zip_test\7-zip.dll)",
      LR"(C:\Users\ewing\Desktop\zip_test\7-zip32.dll)",
      LR"(C:\Users\ewing\Desktop\zip_test\Uninstall.exe)",
  };
  
  std::wstring sfx_path = LR"(C:\Users\ewing\Desktop\zip_test\output_sfx.exe)";
  
  // 删除已存在的文件
  if (::PathFileExists(sfx_path.c_str())) {
    ::DeleteFile(sfx_path.c_str());
  }
  
  // 设置密码（可选）
  std::wstring password = L"";
  
  std::wcout << L"Creating self-extracting archive..." << std::endl;
  
  // 创建自解压文件
  bool success = FileCompressor::CreateSelfExtractingArchive(files, sfx_path, password);
  
  if (success) {
    if (::PathFileExists(sfx_path.c_str())) {
      std::wcout << L"Self-extracting archive created successfully!" << std::endl;
      std::wcout << L"File: " << sfx_path << std::endl;
      std::wcout << L"Password: " << password << std::endl;
      return 0; // Success
    }
  }
  
  std::wcout << L"Failed to create self-extracting archive!" << std::endl;
  return 1; // Failure
}
