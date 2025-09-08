#include "StdAfx.h"
#include <vector>
#include <string>
#include "cpp/Common/MyWindows.h"
#include "cpp/Common/MyInitGuid.h"
#include "cpp/Common/Defs.h"
#include "cpp/Common/StringConvert.h"
#include "cpp/Common/IntToString.h"
#include "cpp/Windows/DLL.h"
#include "cpp/Windows/FileDir.h"
#include "cpp/Windows/FileFind.h"
#include "cpp/Windows/PropVariant.h"
#include "cpp/Windows/PropVariantConv.h"
#include "cpp/Windows/TimeUtils.h"
#include "cpp/7zip/Common/FileStreams.h"
#include "cpp/7zip/Common/LimitedStreams.h"
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
  // 分卷压缩相关成员
  CRecordVector<UInt64> VolumesSizes;
  UString VolName;
  UString VolExt;
  
  const CObjectVector<CDirItem> *dir_items_;
  bool password_is_defined_;
  UString password_;
  
  CArchiveUpdateCallback(): dir_items_(NULL), password_is_defined_(false) {}
  
  void Init(const CObjectVector<CDirItem> *dir_items, const UString &password) {
    dir_items_ = dir_items;
    password_ = password;
    password_is_defined_ = !password.IsEmpty();
  }
  
  // 设置分卷参数
  void SetVolumeInfo(const UString &vol_name, const UString &vol_ext, UInt64 volume_size) {
    VolName = vol_name;
    VolExt = vol_ext;
    VolumesSizes.Clear();
    if (volume_size > 0) {
      VolumesSizes.Add(volume_size);
    }
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
    case kpidSize: prop = dir_item.Size; break;
    case kpidCTime: PropVariant_SetFrom_FiTime(prop, dir_item.CTime); break;
    case kpidATime: PropVariant_SetFrom_FiTime(prop, dir_item.ATime); break;
    case kpidMTime: PropVariant_SetFrom_FiTime(prop, dir_item.MTime); break;
    case kpidAttrib: prop = (UInt32)dir_item.GetWinAttrib(); break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetStream(UInt32 index, ISequentialInStream **in_stream)) {
  const CDirItem &dir_item = (*dir_items_)[index];
  if (dir_item.IsDir()) return S_OK;
  
  CInFileStream *in_stream_spec = new CInFileStream;
  CMyComPtr<ISequentialInStream> in_stream_loc(in_stream_spec);
  if (!in_stream_spec->Open(dir_item.full_path)) return E_FAIL;
  *in_stream = in_stream_loc.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::SetOperationResult(Int32 operationResult)) {
  return S_OK;
}

// 分卷大小获取方法
Z7_COM7F_IMF(CArchiveUpdateCallback::GetVolumeSize(UInt32 index, UInt64 *size)) {
  if (VolumesSizes.IsEmpty())
    return S_FALSE;
  if (index >= (UInt32)VolumesSizes.Size())
    *size = VolumesSizes.Back();
  else
    *size = VolumesSizes[index];
  return S_OK;
}

// 分卷流创建方法
Z7_COM7F_IMF(CArchiveUpdateCallback::GetVolumeStream(UInt32 index, ISequentialOutStream **volumeStream)) {
  wchar_t temp[16];
  ConvertUInt32ToString(index + 1, temp);
  UString res = temp;
  while (res.Len() < 3)  // 使用3位数字，支持更多分卷
    res.InsertAtFront(L'0');
  UString fileName = VolName;
  fileName.Add_Dot();
  fileName += res;
  if (!VolExt.IsEmpty()) {
    fileName.Add_Dot();
    fileName += VolExt;
  }
  
  COutFileStream *streamSpec = new COutFileStream;
  CMyComPtr<ISequentialOutStream> streamLoc(streamSpec);
  if (!streamSpec->Create_NEW(us2fs(fileName)))
    return GetLastError_noZero_HRESULT();
  *volumeStream = streamLoc.Detach();
  
  std::wcout << L"Creating volume: " << fileName << std::endl;
  return S_OK;
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
    return CreateArchive(file_paths, archive_path, password, false, 0);
  }
  
  // 创建分卷压缩文件
  static bool CompressFilesWithVolumes(const std::vector<std::wstring>& file_paths,
                                      const std::wstring& base_archive_path,
                                      UInt64 volume_size_mb,
                                      const std::wstring& password = L"") {
    if (volume_size_mb == 0) {
      std::wcerr << L"Volume size must be greater than 0" << std::endl;
      return false;
    }
    
    UInt64 volume_size_bytes = volume_size_mb * 1024 * 1024;  // 转换为字节
    return CreateArchive(file_paths, base_archive_path, password, false, volume_size_bytes);
  }
  
  // 创建自解压文件
  static bool CreateSelfExtractingArchive(const std::vector<std::wstring>& file_paths,
                                         const std::wstring& sfx_path,
                                         const std::wstring& password = L"",
                                         const std::wstring& sfx_module_path = L"") {
    // 生成临时7z文件
    std::wstring temp_7z_path = sfx_path + L".temp.7z";
    
    if (!CreateArchive(file_paths, temp_7z_path, password, true, 0)) {
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
                           bool for_sfx,
                           UInt64 volume_size = 0) {
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
    
    // Create output file or volume stream
    CMyComPtr<IOutStream> out_file_stream;
    
    if (volume_size > 0) {
      // 分卷压缩：创建第一个分卷文件
      FString archive_name = us2fs(UString(archive_path.c_str()));
      COutFileStream *out_file_stream_spec = new COutFileStream;
      
      // 创建一个有限制大小的输出流
      CLimitedSequentialOutStream *limited_out_stream_spec = new CLimitedSequentialOutStream;
      CMyComPtr<ISequentialOutStream> limited_out_stream(limited_out_stream_spec);
      
      // 打开第一个分卷文件
      if (!out_file_stream_spec->Create_NEW(archive_name)) return false;
      
      // 设置有限制大小的输出流
      limited_out_stream_spec->SetStream(out_file_stream_spec);
      limited_out_stream_spec->Init(volume_size);
      
      // 将有限制大小的输出流转换为IOutStream接口
      // 注意：这里需要一个支持Seek操作的流，所以我们仍然需要使用原始的out_file_stream
      out_file_stream = out_file_stream_spec;
      
      std::wcout << L"Creating multi-volume archive with volume size: "
                 << (volume_size / 1024 / 1024) << L" MB" << std::endl;
    } else {
      // 普通压缩：创建单一输出文件
      FString archive_name = us2fs(UString(archive_path.c_str()));
      COutFileStream *out_file_stream_spec = new COutFileStream;
      out_file_stream = out_file_stream_spec;
      if (!out_file_stream_spec->Create_NEW(archive_name)) return false;
    }
    
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
      std::cout << "set_ret: " << set_ret;
    }
    
    // Create update callback
    CArchiveUpdateCallback* update_callback_spec = new CArchiveUpdateCallback;
    CMyComPtr<IArchiveUpdateCallback2> update_callback(update_callback_spec);
    update_callback_spec->Init(&dir_items, UString(password.c_str()));
    
    // 设置分卷信息
    if (volume_size > 0) {
      // 从archive_path提取基础名称和扩展名
      UString base_path = UString(archive_path.c_str());
      int dot_pos = base_path.ReverseFind_Dot();
      UString vol_name, vol_ext;
      
      if (dot_pos >= 0) {
        vol_name = base_path.Left(dot_pos);
        vol_ext = base_path.Ptr(dot_pos + 1);
      } else {
        vol_name = base_path;
        vol_ext = L"7z";
      }
      
      update_callback_spec->SetVolumeInfo(vol_name, vol_ext, volume_size);
    }
    
    // Perform compression
    HRESULT result = out_archive->UpdateItems(out_file_stream, 
                                             dir_items.Size(), 
                                             update_callback);
    
    if (result == S_OK) {
      if (volume_size > 0) {
        std::wcout << L"Multi-volume archive created successfully!" << std::endl;
      } else {
        std::wcout << L"Archive created: " << archive_path << std::endl;
      }
    }
    
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
  std::wcout << L"=== 7z Compression Demo with Volume Support ===" << std::endl;
  
  // 测试文件列表
  std::vector<std::wstring> test_files = {
    LR"(C:\Users\ewing\Desktop\zip_test\7zFM.exe)",
    LR"(C:\Users\ewing\Desktop\zip_test\7zG.exe)",
    LR"(C:\Users\ewing\Desktop\zip_test\7-zip.chm)",
    LR"(C:\Users\ewing\Desktop\zip_test\output.amv)"
  };
  
  // 1. 测试普通压缩
  // std::wcout << L"\n1. Testing normal compression..." << std::endl;
  // if (FileCompressor::CompressFiles(test_files,
  // L"C:\\Users\\ewing\\Desktop\\archive_temp\\normal_archive.7z")) {
  //   std::wcout << L"Normal compression successful!" << std::endl;
  // } else {
  //   std::wcout << L"Normal compression failed!" << std::endl;
  // }
  
  // 2. 测试分卷压缩 (每个分卷5MB)
  std::wstring out_file = L"C:\\Users\\ewing\\Desktop\\zip_test\\volume_archive.7z";
  if (PathFileExists(out_file.c_str())) {
    DeleteFile(out_file.c_str());
  }
  std::wcout << L"\n2. Testing volume compression (5MB per volume)..." << std::endl;
  if (FileCompressor::CompressFilesWithVolumes(test_files, 
                                              out_file.c_str(), 
                                              5)) {  // 5MB per volume
    std::wcout << L"Volume compression successful!" << std::endl;
  } else {
    std::wcout << L"Volume compression failed!" << std::endl;
  }
  
  // 3. 测试自解压文件
  // std::wcout << L"\n3. Testing self-extracting archive..." << std::endl;
  // if (FileCompressor::CreateSelfExtractingArchive(test_files, 
  //                                                 L"C:\\Users\\ewing\\Desktop\\archive_temp\\selfextract.exe")) {
  //   std::wcout << L"Self-extracting archive created successfully!" << std::endl;
  // } else {
  //   std::wcout << L"Self-extracting archive creation failed!" << std::endl;
  // }
  
  std::wcout << L"\n=== Demo completed ===" << std::endl;
  return 0;
}
