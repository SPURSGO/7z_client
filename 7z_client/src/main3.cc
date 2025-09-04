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
  static bool CompressFiles(const std::vector<std::wstring>& file_paths, 
                           const std::wstring& archive_path,
                           const std::wstring& password = L"") {
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
    
    // Set compression method and encryption if password is provided
    if (!password.empty()) {
      // Query ISetProperties interface
      CMyComPtr<ISetProperties> set_properties;
      if (out_archive->QueryInterface(IID_ISetProperties, (void**)&set_properties) == S_OK) {
        const wchar_t *names[] = {L"x", L"he"};
        PROPVARIANT values_xx[2] = {};
        for (int i = 0; i < 2; i++) PropVariantInit(&values_xx[i]);
        // 压缩等级
        values_xx[0].vt = VT_UI4;
        values_xx[0].ulVal = 9;

        // 加密方法 AES-256  7Z内部默认使用AES-256加密。
        // 所以只需要实现 ICryptoGetTextPassword2 接口即可
        //values_xx[1].vt = VT_BSTR;
        //values_xx[1].bstrVal = SysAllocString(L"AES256");

        // 加密文件名
        values_xx[1].vt = VT_BOOL;
        values_xx[1].boolVal = VARIANT_TRUE;

        HRESULT hrxx = set_properties->SetProperties(names, values_xx, 2);
        
        if (hrxx != S_OK) return false;
      }
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
};

int main() {
  // Example usage with wide strings and password protection
  std::vector<std::wstring> files = {
      LR"(C:\Users\ewing\Desktop\7z_test\7zFM.exe)",
      LR"(C:\Users\ewing\Desktop\7z_test\7zG.exe)",
      LR"(C:\Users\ewing\Desktop\7z_test\7-zip.chm)",
      LR"(C:\Users\ewing\Desktop\7z_test\7-zip.dll)",
      LR"(C:\Users\ewing\Desktop\7z_test\7-zip32.dll)",
      LR"(C:\Users\ewing\Desktop\7z_test\Uninstall.exe)",
  };
  
  std::wstring archive_path = LR"(C:\Users\ewing\Desktop\archive_temp\output.7z)";
  
  // Delete existing file if present
  if (::PathFileExists(archive_path.c_str())) {
    ::DeleteFile(archive_path.c_str());
  }
  
  // Set password for encryption
  std::wstring password = L"mypassword123";
  
  bool success = FileCompressor::CompressFiles(files, archive_path, password);
  
  if (success) {
    // Verify the encrypted archive was created
    if (::PathFileExists(archive_path.c_str())) {
      return 0; // Success
    }
  }
  
  return 1; // Failure
}
